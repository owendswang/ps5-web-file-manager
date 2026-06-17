#include "filemgr.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#ifdef __linux__
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#endif
#include <time.h>
#include <unistd.h>

#include "mime.h"
#include "websrv.h"

#define COPY_BUFFER_SIZE (8 * 1024 * 1024)
typedef struct strbuf {
  char *data;
  size_t len;
  size_t cap;
} strbuf_t;

typedef enum task_op {
  TASK_COPY,
  TASK_MOVE,
  TASK_DELETE,
} task_op_t;

typedef enum task_state {
  TASK_QUEUED,
  TASK_RUNNING,
  TASK_DONE,
  TASK_FAILED,
  TASK_CANCELED,
} task_state_t;

typedef struct file_task {
  unsigned long id;
  task_op_t op;
  task_state_t state;
  char src[PATH_MAX];
  char dst[PATH_MAX];
  char current[PATH_MAX];
  char error[160];
  char error_code[64];
  char error_arg[PATH_MAX];
  char **srcs;
  size_t src_count;
  unsigned long long total;
  unsigned long long done;
  unsigned long long speed;
  unsigned long long speed_sample_done;
  struct timespec speed_sample_time;
  int cancel_requested;
  time_t created_at;
  time_t transfer_started_at;
  time_t updated_at;
  pthread_t thread;
  struct file_task *next;
} file_task_t;

static pthread_mutex_t g_tasks_lock = PTHREAD_MUTEX_INITIALIZER;
static file_task_t *g_tasks = NULL;
static unsigned long g_next_task_id = 1;

static int
strbuf_reserve(strbuf_t *b, size_t extra) {
  size_t need = b->len + extra + 1;
  char *tmp;

  if(need <= b->cap) {
    return 0;
  }

  size_t cap = b->cap ? b->cap : 4096;
  while(cap < need) {
    cap *= 2;
  }

  if(!(tmp = realloc(b->data, cap))) {
    return -1;
  }

  b->data = tmp;
  b->cap = cap;
  return 0;
}

static int
strbuf_append(strbuf_t *b, const char *s) {
  size_t n = strlen(s);
  if(strbuf_reserve(b, n)) {
    return -1;
  }
  memcpy(b->data + b->len, s, n + 1);
  b->len += n;
  return 0;
}

static int
strbuf_printf(strbuf_t *b, const char *fmt, ...) {
  va_list ap;
  va_list cp;
  int n;

  va_start(ap, fmt);
  va_copy(cp, ap);
  n = vsnprintf(NULL, 0, fmt, cp);
  va_end(cp);
  if(n < 0 || strbuf_reserve(b, (size_t)n)) {
    va_end(ap);
    return -1;
  }

  vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
  return 0;
}

static char *
query_value(struct MHD_Connection *conn, const char *key) {
  const char *raw = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, key);
  return raw ? strdup(raw) : NULL;
}

static void
free_paths(char **paths, size_t count) {
  size_t i;

  if(!paths) {
    return;
  }
  for(i = 0; i < count; i++) {
    free(paths[i]);
  }
  free(paths);
}

static int
parse_paths(const char *raw, char ***out_paths, size_t *out_count) {
  char *copy;
  char *line;
  char *save;
  char **paths = NULL;
  size_t count = 0;

  *out_paths = NULL;
  *out_count = 0;

  if(!raw || !raw[0]) {
    errno = EINVAL;
    return -1;
  }
  if(!(copy = strdup(raw))) {
    errno = ENOMEM;
    return -1;
  }

  for(line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    char **tmp;
    if(!line[0]) {
      continue;
    }
    if(!(tmp = realloc(paths, sizeof(char *) * (count + 1)))) {
      free_paths(paths, count);
      free(copy);
      errno = ENOMEM;
      return -1;
    }
    paths = tmp;
    if(!(paths[count] = strdup(line))) {
      free_paths(paths, count);
      free(copy);
      errno = ENOMEM;
      return -1;
    }
    count++;
  }

  free(copy);
  if(!count) {
    errno = EINVAL;
    return -1;
  }

  *out_paths = paths;
  *out_count = count;
  return 0;
}

static const char *
path_basename(const char *path) {
  const char *end = path + strlen(path);
  const char *base;

  while(end > path && end[-1] == '/') {
    end--;
  }
  base = end;
  while(base > path && base[-1] != '/') {
    base--;
  }
  return base;
}

static int
path_dirname(const char *path, char *out, size_t size) {
  const char *base = path_basename(path);
  size_t len = (size_t)(base - path);

  while(len > 1 && path[len - 1] == '/') {
    len--;
  }
  if(!len) {
    len = 1;
  }
  if(len >= size) {
    return -1;
  }
  memcpy(out, path, len);
  out[len] = 0;
  return 0;
}

static int
path_join(char *out, size_t size, const char *dir, const char *name) {
  int n;
  if(!dir || !name || !dir[0] || !name[0] || strchr(name, '/')) {
    errno = EINVAL;
    return -1;
  }
  n = snprintf(out, size, "%s%s%s", dir,
               (strcmp(dir, "/") && dir[strlen(dir) - 1] != '/') ? "/" : "",
               name);
  if(n < 0 || (size_t)n >= size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

static char
mode_type(const struct stat *st) {
  if(S_ISDIR(st->st_mode)) return 'd';
  if(S_ISLNK(st->st_mode)) return 'l';
  if(S_ISCHR(st->st_mode)) return 'c';
  if(S_ISBLK(st->st_mode)) return 'b';
  if(S_ISFIFO(st->st_mode)) return 'p';
  if(S_ISSOCK(st->st_mode)) return 's';
  return '-';
}

static const char *
task_op_name(task_op_t op) {
  switch(op) {
  case TASK_COPY: return "copy";
  case TASK_MOVE: return "move";
  case TASK_DELETE: return "delete";
  default: return "unknown";
  }
}

static const char *
task_state_name(task_state_t state) {
  switch(state) {
  case TASK_QUEUED: return "queued";
  case TASK_RUNNING: return "running";
  case TASK_DONE: return "done";
  case TASK_FAILED: return "failed";
  case TASK_CANCELED: return "canceled";
  default: return "unknown";
  }
}

static int
task_is_active(const file_task_t *task) {
  return task->state == TASK_QUEUED || task->state == TASK_RUNNING;
}

static int
has_active_task_locked(void) {
  file_task_t *task;

  for(task = g_tasks; task; task = task->next) {
    if(task_is_active(task)) {
      return 1;
    }
  }
  return 0;
}

static int
has_active_task(void) {
  int active;

  pthread_mutex_lock(&g_tasks_lock);
  active = has_active_task_locked();
  pthread_mutex_unlock(&g_tasks_lock);
  return active;
}

static void
free_task(file_task_t *task) {
  if(!task) {
    return;
  }
  free_paths(task->srcs, task->src_count);
  free(task);
}

static void
remove_finished_tasks_locked(void) {
  file_task_t **link = &g_tasks;

  while(*link) {
    file_task_t *task = *link;

    if(task_is_active(task)) {
      link = &task->next;
      continue;
    }

    *link = task->next;
    free_task(task);
  }
}

static int
task_cancel_requested(file_task_t *task) {
  int cancel;

  pthread_mutex_lock(&g_tasks_lock);
  cancel = task->cancel_requested;
  pthread_mutex_unlock(&g_tasks_lock);
  if(cancel) {
    errno = ECANCELED;
  }
  return cancel;
}

static void
task_update(file_task_t *task, task_state_t state, const char *current,
            unsigned long long add_done, const char *error) {
  struct timespec now_mono;
  time_t now;

  clock_gettime(CLOCK_MONOTONIC, &now_mono);
  now = time(NULL);

  pthread_mutex_lock(&g_tasks_lock);
  task->state = state;
  if(current) {
    snprintf(task->current, sizeof(task->current), "%s", current);
  }
  if(add_done) {
    if(!task->transfer_started_at) {
      task->transfer_started_at = now;
    }
    task->done += add_done;
    if(task->total && task->done > task->total) {
      task->done = task->total;
    }
    if(task->speed_sample_time.tv_sec) {
      long long elapsed_ns =
        (long long)(now_mono.tv_sec - task->speed_sample_time.tv_sec) * 1000000000LL +
        (long long)(now_mono.tv_nsec - task->speed_sample_time.tv_nsec);
      if(elapsed_ns >= 250000000LL) {
        unsigned long long delta = task->done - task->speed_sample_done;
        task->speed = (unsigned long long)((delta * 1000000000ULL) /
                                           (unsigned long long)elapsed_ns);
        task->speed_sample_done = task->done;
        task->speed_sample_time = now_mono;
      }
    } else {
      task->speed_sample_done = task->done;
      task->speed_sample_time = now_mono;
    }
  }
  if(error) {
    snprintf(task->error, sizeof(task->error), "%s", error);
  }
  task->updated_at = now;
  pthread_mutex_unlock(&g_tasks_lock);
}

static void
task_set_error_code(file_task_t *task, const char *code, const char *arg) {
  pthread_mutex_lock(&g_tasks_lock);
  if(code) {
    snprintf(task->error_code, sizeof(task->error_code), "%s", code);
  }
  if(arg) {
    snprintf(task->error_arg, sizeof(task->error_arg), "%s", arg);
  }
  pthread_mutex_unlock(&g_tasks_lock);
}

static void
task_set_total(file_task_t *task, unsigned long long total) {
  pthread_mutex_lock(&g_tasks_lock);
  task->total = total;
  task->updated_at = time(NULL);
  pthread_mutex_unlock(&g_tasks_lock);
}

static void
task_finish_bytes(file_task_t *task, const char *current) {
  pthread_mutex_lock(&g_tasks_lock);
  task->state = TASK_RUNNING;
  if(current) {
    snprintf(task->current, sizeof(task->current), "%s", current);
  }
  if(task->total) {
    task->done = task->total;
  }
  task->speed = 0;
  task->updated_at = time(NULL);
  pthread_mutex_unlock(&g_tasks_lock);
}

static int
json_escape(strbuf_t *b, const char *s) {
  if(strbuf_append(b, "\"")) return -1;
  for(; *s; s++) {
    unsigned char c = (unsigned char)*s;
    switch(c) {
    case '"': if(strbuf_append(b, "\\\"")) return -1; break;
    case '\\': if(strbuf_append(b, "\\\\")) return -1; break;
    case '\b': if(strbuf_append(b, "\\b")) return -1; break;
    case '\f': if(strbuf_append(b, "\\f")) return -1; break;
    case '\n': if(strbuf_append(b, "\\n")) return -1; break;
    case '\r': if(strbuf_append(b, "\\r")) return -1; break;
    case '\t': if(strbuf_append(b, "\\t")) return -1; break;
    default:
      if(c < 0x20) {
        if(strbuf_printf(b, "\\u%04x", c)) return -1;
      } else {
        if(strbuf_reserve(b, 1)) return -1;
        b->data[b->len++] = (char)c;
        b->data[b->len] = 0;
      }
      break;
    }
  }
  return strbuf_append(b, "\"");
}

static enum MHD_Result
send_buffer(struct MHD_Connection *conn, unsigned int status, char *data,
            const char *mime) {
  struct MHD_Response *resp;
  enum MHD_Result ret = MHD_NO;
  size_t len = data ? strlen(data) : 0;

  if((resp = MHD_create_response_from_buffer(len, data ? data : "",
                                             data ? MHD_RESPMEM_MUST_FREE :
                                                    MHD_RESPMEM_PERSISTENT))) {
    if(mime) {
      MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
    }
    ret = websrv_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  } else {
    free(data);
  }

  return ret;
}

static enum MHD_Result
send_json_ok(struct MHD_Connection *conn) {
  return send_buffer(conn, MHD_HTTP_OK, strdup("{\"ok\":true}"),
                     "application/json");
}

static const char *
api_error_code(const char *msg) {
  if(!msg) return "system_error";
  if(!strcmp(msg, "another task is running")) return "active_task";
  if(!strcmp(msg, "active task not found")) return "active_task_not_found";
  if(!strcmp(msg, "source and destination are the same")) return "source_destination_same";
  if(!strcmp(msg, "destination is inside source directory")) return "destination_inside_source";
  if(!strcmp(msg, "invalid path")) return "invalid_path";
  if(!strcmp(msg, "unknown api")) return "unknown_api";
  if(!strcmp(msg, "out of memory")) return "out_of_memory";
  if(!strcmp(msg, "no source paths")) return "no_source_paths";
  if(!strcmp(msg, "destination must be a directory for multiple items")) {
    return "destination_must_be_directory";
  }
  return "system_error";
}

static enum MHD_Result
send_json_error(struct MHD_Connection *conn, unsigned int status,
                const char *msg) {
  strbuf_t b = {0};
  const char *fallback = msg ? msg : strerror(errno);

  strbuf_append(&b, "{\"ok\":false,\"error\":");
  json_escape(&b, fallback);
  strbuf_append(&b, ",\"error_code\":");
  json_escape(&b, api_error_code(msg));
  strbuf_append(&b, ",\"error_arg\":");
  json_escape(&b, fallback);
  strbuf_append(&b, "}");
  return send_buffer(conn, status, b.data, "application/json");
}

static int count_path_bytes(file_task_t *task, const char *path,
                            const char *display,
                            unsigned long long *total);
static int task_target_path(file_task_t *task, const char *src,
                            char *out, size_t size);

static int
count_dir_bytes(file_task_t *task, const char *path, const char *display,
                unsigned long long *total) {
  DIR *dir = opendir(path);
  struct dirent *entry;
  int ret = -1;

  if(!dir) {
    return -1;
  }
  while((entry = readdir(dir))) {
    char child[PATH_MAX];
    char display_child[PATH_MAX];
    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(task && task_cancel_requested(task)) {
      goto done;
    }
    if(path_join(child, sizeof(child), path, entry->d_name) ||
       (display && path_join(display_child, sizeof(display_child), display,
                             entry->d_name)) ||
       count_path_bytes(task, child, display ? display_child : NULL, total)) {
      goto done;
    }
  }
  ret = 0;
done:
  closedir(dir);
  return ret;
}

static int
count_path_bytes(file_task_t *task, const char *path, const char *display,
                 unsigned long long *total) {
  struct stat st;

  if(task && task_cancel_requested(task)) {
    return -1;
  }
  if(task) {
    task_update(task, TASK_RUNNING, display ? display : path, 0, NULL);
  }

  if(lstat(path, &st)) {
    return -1;
  }
  if(S_ISDIR(st.st_mode)) {
    return count_dir_bytes(task, path, display, total);
  }
  if(S_ISREG(st.st_mode)) {
    *total += (unsigned long long)st.st_size;
  }
  return 0;
}

static int
ignore_chmod_error(int err) {
  return err == ENOTSUP || err == EPERM || err == EINVAL || err == EROFS;
}

#ifndef __linux__
static int
fs_type_has_unix_modes(const char *type) {
  return strcmp(type, "exfat") &&
         strcmp(type, "exfatfs") &&
         strcmp(type, "msdosfs") &&
         strcmp(type, "fat") &&
         strcmp(type, "vfat");
}
#endif

static int
path_has_unix_modes(const char *path) {
#ifdef __linux__
  (void)path;
  return 1;
#else
  struct statfs fs;

  if(statfs(path, &fs)) {
    return 1;
  }
  return fs_type_has_unix_modes(fs.f_fstypename);
#endif
}

static int
fd_has_unix_modes(int fd) {
#ifdef __linux__
  (void)fd;
  return 1;
#else
  struct statfs fs;

  if(fstatfs(fd, &fs)) {
    return 1;
  }
  return fs_type_has_unix_modes(fs.f_fstypename);
#endif
}

static int
chmod_path_0777(const char *path) {
  if(!path_has_unix_modes(path)) {
    return 0;
  }
  if(chmod(path, 0777) && !ignore_chmod_error(errno)) {
    return -1;
  }
  return 0;
}

static int
fchmod_0777(int fd) {
  if(!fd_has_unix_modes(fd)) {
    return 0;
  }
  if(fchmod(fd, 0777) && !ignore_chmod_error(errno)) {
    return -1;
  }
  return 0;
}

static int
chmod_tree_0777(const char *path) {
  struct stat st;

  if(lstat(path, &st)) {
    return -1;
  }
  if(S_ISDIR(st.st_mode)) {
    DIR *dir = opendir(path);
    struct dirent *entry;
    if(!dir) {
      return -1;
    }
    while((entry = readdir(dir))) {
      char child[PATH_MAX];
      if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
        continue;
      }
      if(path_join(child, sizeof(child), path, entry->d_name) ||
         chmod_tree_0777(child)) {
        closedir(dir);
        return -1;
      }
    }
    closedir(dir);
  } else if(!S_ISREG(st.st_mode)) {
    return 0;
  }
  return chmod_path_0777(path);
}

static int
copy_file(file_task_t *task, const char *src, const char *dst, mode_t mode) {
  char *buf = NULL;
  int in = -1;
  int out = -1;
  int ret = -1;
  ssize_t n;

  task_update(task, TASK_RUNNING, dst, 0, NULL);

  if(task_cancel_requested(task)) {
    return -1;
  }
  if((in = open(src, O_RDONLY)) < 0) {
    goto done;
  }
  (void)mode;
  if((out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0777)) < 0) {
    goto done;
  }
  if(!(buf = malloc(COPY_BUFFER_SIZE))) {
    errno = ENOMEM;
    goto done;
  }

  while((n = read(in, buf, COPY_BUFFER_SIZE)) > 0) {
    ssize_t left = n;
    char *p = buf;
    if(task_cancel_requested(task)) {
      goto done;
    }
    while(left > 0) {
      ssize_t w = write(out, p, (size_t)left);
      if(w <= 0) {
        goto done;
      }
      p += w;
      left -= w;
      task_update(task, TASK_RUNNING, dst, (unsigned long long)w, NULL);
    }
  }
  if(n < 0) {
    goto done;
  }
  if(fchmod_0777(out)) {
    goto done;
  }
  ret = 0;

done:
  free(buf);
  if(in >= 0) close(in);
  if(out >= 0) {
    if(close(out)) ret = -1;
  }
  if(ret) unlink(dst);
  return ret;
}

static int copy_path(file_task_t *task, const char *src, const char *dst);
static int remove_path(file_task_t *task, const char *path);
static int check_remove_path_writable(file_task_t *task, const char *path);

static int
check_remove_entry_writable(const char *path) {
  char parent[PATH_MAX];

  if(path_dirname(path, parent, sizeof(parent))) {
    return -1;
  }
  return access(parent, W_OK | X_OK);
}

static int
check_remove_dir_writable(file_task_t *task, const char *path) {
  DIR *dir;
  struct dirent *entry;
  int ret = -1;

  if(access(path, R_OK | W_OK | X_OK)) {
    task_update(task, TASK_RUNNING, path, 0, NULL);
    return -1;
  }
  if(!(dir = opendir(path))) {
    task_update(task, TASK_RUNNING, path, 0, NULL);
    return -1;
  }
  while((entry = readdir(dir))) {
    char child[PATH_MAX];

    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(task_cancel_requested(task)) {
      goto done;
    }
    if(path_join(child, sizeof(child), path, entry->d_name) ||
       check_remove_path_writable(task, child)) {
      goto done;
    }
  }
  ret = 0;
done:
  closedir(dir);
  return ret;
}

static int
check_remove_path_writable(file_task_t *task, const char *path) {
  struct stat st;

  if(task_cancel_requested(task)) {
    return -1;
  }
  if(lstat(path, &st)) {
    task_update(task, TASK_RUNNING, path, 0, NULL);
    return -1;
  }
  if(check_remove_entry_writable(path)) {
    task_update(task, TASK_RUNNING, path, 0, NULL);
    return -1;
  }
  if(S_ISDIR(st.st_mode)) {
    return check_remove_dir_writable(task, path);
  }
  return 0;
}

static int
finish_copied_move(file_task_t *task, const char *src, const char *dst) {
  int ret;

  task_finish_bytes(task, "setting permissions");
  ret = chmod_tree_0777(dst);
  if(!ret) {
    task_finish_bytes(task, "checking source permissions");
    ret = check_remove_path_writable(task, src);
  }
  if(!ret) {
    task_finish_bytes(task, "removing source");
    ret = remove_path(task, src);
  }
  return ret;
}

static int
copy_dir(file_task_t *task, const char *src, const char *dst, mode_t mode) {
  DIR *dir;
  struct dirent *entry;
  int created = 0;
  int ret = -1;

  (void)mode;
  task_update(task, TASK_RUNNING, dst, 0, NULL);

  if(task_cancel_requested(task)) {
    return -1;
  }
  if(mkdir(dst, 0777)) {
    if(errno != EEXIST) {
      return -1;
    }
  } else {
    created = 1;
  }
  if(created && chmod_path_0777(dst)) {
    return -1;
  }
  if(!(dir = opendir(src))) {
    return -1;
  }

  while((entry = readdir(dir))) {
    char from[PATH_MAX];
    char to[PATH_MAX];

    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(task_cancel_requested(task)) {
      goto done;
    }
    if(path_join(from, sizeof(from), src, entry->d_name) ||
       path_join(to, sizeof(to), dst, entry->d_name) ||
       copy_path(task, from, to)) {
      goto done;
    }
  }

  ret = 0;
done:
  closedir(dir);
  return ret;
}

static int
copy_path(file_task_t *task, const char *src, const char *dst) {
  struct stat st;

  if(lstat(src, &st)) {
    return -1;
  }
  if(S_ISDIR(st.st_mode)) {
    return copy_dir(task, src, dst, st.st_mode & 0777);
  }
  if(S_ISREG(st.st_mode)) {
    return copy_file(task, src, dst, st.st_mode & 0777);
  }
  errno = ENOTSUP;
  return -1;
}

static int
move_path(file_task_t *task, const char *src, const char *dst) {
  struct stat src_st;
  struct stat dst_st;
  int dst_exists;
  int ret;

  if(lstat(src, &src_st)) {
    return -1;
  }
  dst_exists = !lstat(dst, &dst_st);
  task_update(task, TASK_RUNNING, dst, 0, NULL);

  if(dst_exists && S_ISDIR(src_st.st_mode) && S_ISDIR(dst_st.st_mode)) {
    ret = copy_path(task, src, dst);
    if(!ret) {
      ret = finish_copied_move(task, src, dst);
    }
    return ret;
  }

  ret = rename(src, dst);
  if(!ret) {
    task_finish_bytes(task, "setting permissions");
    (void)chmod_tree_0777(dst);
  } else if(errno == EXDEV) {
    ret = copy_path(task, src, dst);
    if(!ret) {
      ret = finish_copied_move(task, src, dst);
    }
  }
  return ret;
}

static int
remove_path(file_task_t *task, const char *path) {
  struct stat st;

  task_update(task, TASK_RUNNING, path, 0, NULL);

  if(task_cancel_requested(task)) {
    return -1;
  }
  if(lstat(path, &st)) {
    return -1;
  }
  if(S_ISDIR(st.st_mode)) {
    DIR *dir = opendir(path);
    struct dirent *entry;
    if(!dir) {
      return -1;
    }
    while((entry = readdir(dir))) {
      char child[PATH_MAX];
      if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
        continue;
      }
      if(task_cancel_requested(task)) {
        closedir(dir);
        return -1;
      }
      if(path_join(child, sizeof(child), path, entry->d_name) ||
         remove_path(task, child)) {
        closedir(dir);
        return -1;
      }
    }
    closedir(dir);
    return rmdir(path);
  }
  if(S_ISREG(st.st_mode)) {
    task_update(task, TASK_RUNNING, path, (unsigned long long)st.st_size, NULL);
  }
  return unlink(path);
}

static int
resolve_destination(const char *src, const char *dst, char *out, size_t size) {
  struct stat st;

  if(!stat(dst, &st) && S_ISDIR(st.st_mode)) {
    return path_join(out, size, dst, path_basename(src));
  }
  if(strlen(dst) >= size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  strcpy(out, dst);
  return 0;
}

static int
validate_task_target(const char *src, const char *target, int overwrite,
                     char *error, size_t error_size) {
  struct stat src_st;
  struct stat target_st;

  if(!strcmp(src, target)) {
    snprintf(error, error_size, "source and destination are the same");
    errno = EINVAL;
    return -1;
  }
  if(lstat(src, &src_st)) {
    snprintf(error, error_size, "source not found");
    return -1;
  }
  if(S_ISDIR(src_st.st_mode)) {
    size_t src_len = strlen(src);

    while(src_len > 1 && src[src_len - 1] == '/') {
      src_len--;
    }
    if(!strncmp(src, target, src_len) &&
       (target[src_len] == 0 || target[src_len] == '/')) {
      snprintf(error, error_size, "destination is inside source directory");
      errno = EINVAL;
      return -1;
    }
  }
  if(lstat(target, &target_st)) {
    if(errno == ENOENT) {
      return 0;
    }
    snprintf(error, error_size, "cannot check destination");
    return -1;
  }
  if(src_st.st_dev == target_st.st_dev && src_st.st_ino == target_st.st_ino) {
    snprintf(error, error_size, "source and destination are the same");
    errno = EINVAL;
    return -1;
  }
  if(!overwrite) {
    snprintf(error, error_size, "destination exists");
    errno = EEXIST;
    return -1;
  }
  if(S_ISDIR(src_st.st_mode) != S_ISDIR(target_st.st_mode)) {
    snprintf(error, error_size, "remove conflicting file or directory first");
    errno = EEXIST;
    return -1;
  }
  return 0;
}

static int
target_statvfs(const char *target, struct statvfs *vfs) {
  char parent[PATH_MAX];

  if(!statvfs(target, vfs)) {
    return 0;
  }
  if(path_dirname(target, parent, sizeof(parent))) {
    return -1;
  }
  return statvfs(parent, vfs);
}

static int
move_requires_space_check(const char *src, const char *target) {
  struct stat src_st;
  struct stat target_st;
  struct stat dst_dir_st;
  char parent[PATH_MAX];

  if(lstat(src, &src_st)) {
    return -1;
  }
  if(!lstat(target, &target_st)) {
    if(S_ISDIR(src_st.st_mode) && S_ISDIR(target_st.st_mode)) {
      return 1;
    }
    return src_st.st_dev != target_st.st_dev;
  }
  if(path_dirname(target, parent, sizeof(parent)) || stat(parent, &dst_dir_st)) {
    return -1;
  }
  return src_st.st_dev != dst_dir_st.st_dev;
}

static int
check_target_space(const char *target, unsigned long long required,
                   char *error, size_t error_size,
                   char *code, size_t code_size,
                   char *arg, size_t arg_size) {
  struct statvfs vfs;
  unsigned long long block_size;
  unsigned long long available;

  if(!required) {
    return 0;
  }
  if(target_statvfs(target, &vfs)) {
    snprintf(error, error_size, "cannot read target free space");
    snprintf(code, code_size, "space_check_failed");
    snprintf(arg, arg_size, "%s", target);
    return -1;
  }
  block_size = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
  available = (unsigned long long)vfs.f_bavail * block_size;
  if(available < required) {
    snprintf(error, error_size,
             "not enough target space, required %llu bytes, available %llu bytes",
             required, available);
    snprintf(code, code_size, "no_space");
    snprintf(arg, arg_size, "%llu,%llu", required, available);
    errno = ENOSPC;
    return -1;
  }
  return 0;
}

static void
set_error_detail(char *error, size_t error_size, char *code, size_t code_size,
                 char *arg, size_t arg_size, const char *error_code,
                 const char *message, const char *path) {
  snprintf(error, error_size, "%s: %s", message, path);
  snprintf(code, code_size, "%s", error_code);
  snprintf(arg, arg_size, "%s", path);
}

static int
check_target_writable(const char *target, char *error, size_t error_size,
                      char *code, size_t code_size, char *arg, size_t arg_size) {
  struct stat st;
  char parent[PATH_MAX];

  if(!stat(target, &st)) {
    if(S_ISDIR(st.st_mode)) {
      if(access(target, W_OK | X_OK)) {
        set_error_detail(error, error_size, code, code_size, arg, arg_size,
                         "target_dir_not_writable", "target directory is not writable", target);
        return -1;
      }
    } else {
      if(access(target, W_OK)) {
        set_error_detail(error, error_size, code, code_size, arg, arg_size,
                         "target_file_not_writable", "target file is not writable", target);
        return -1;
      }
      if(path_dirname(target, parent, sizeof(parent)) ||
         access(parent, W_OK | X_OK)) {
        set_error_detail(error, error_size, code, code_size, arg, arg_size,
                         "target_parent_not_writable", "target parent directory is not writable", target);
        return -1;
      }
    }
    return 0;
  }

  if(errno != ENOENT) {
    set_error_detail(error, error_size, code, code_size, arg, arg_size,
                     "target_check_failed", "cannot check target path", target);
    return -1;
  }
  if(path_dirname(target, parent, sizeof(parent)) ||
     access(parent, W_OK | X_OK)) {
    set_error_detail(error, error_size, code, code_size, arg, arg_size,
                     "target_parent_not_writable", "target parent directory is not writable", target);
    return -1;
  }
  return 0;
}

static int
check_task_targets_writable(file_task_t *task, char *error, size_t error_size,
                            char *code, size_t code_size, char *arg, size_t arg_size) {
  size_t i;

  for(i = 0; i < task->src_count; i++) {
    char target[PATH_MAX];

    if(task_cancel_requested(task)) {
      return -1;
    }
    if(task_target_path(task, task->srcs[i], target, sizeof(target))) {
      snprintf(error, error_size, "target path is too long");
      snprintf(code, code_size, "target_path_too_long");
      return -1;
    }
    if(check_target_writable(target, error, error_size, code, code_size, arg, arg_size)) {
      return -1;
    }
  }
  return 0;
}

static unsigned long long
vfs_bytes(fsblkcnt_t blocks, unsigned long block_size) {
  return (unsigned long long)blocks * (unsigned long long)block_size;
}

static int
path_is_mounted(const char *path, const struct stat *st) {
  char parent[PATH_MAX];
  struct stat parent_st;

  if(!strcmp(path, "/")) {
    return 1;
  }
  if(path_dirname(path, parent, sizeof(parent)) || stat(parent, &parent_st)) {
    return 0;
  }
  return st->st_dev != parent_st.st_dev;
}

static int
task_target_path(file_task_t *task, const char *src, char *out, size_t size) {
  if(task->src_count > 1) {
    return path_join(out, size, task->dst, path_basename(src));
  }
  if(strlen(task->dst) >= size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  strcpy(out, task->dst);
  return 0;
}

static int
request_target_path(const char *src, const char *dst, size_t src_count,
                    char *out, size_t size) {
  if(src_count > 1) {
    return path_join(out, size, dst, path_basename(src));
  }
  return resolve_destination(src, dst, out, size);
}

static enum MHD_Result
task_request_error(struct MHD_Connection *conn, file_task_t *task,
                   char **srcs, size_t src_count,
                   unsigned int status, const char *msg) {
  free_paths(srcs, src_count);
  free(task);
  return send_json_error(conn, status, msg);
}

static int
count_required_move_space(file_task_t *task, unsigned long long *required) {
  size_t i;

  *required = 0;
  for(i = 0; i < task->src_count; i++) {
    char target[PATH_MAX];
    int needs_space;

    if(task_cancel_requested(task)) {
      return -1;
    }
    if(task_target_path(task, task->srcs[i], target, sizeof(target))) {
      return -1;
    }
    needs_space = move_requires_space_check(task->srcs[i], target);
    if(needs_space < 0) {
      return -1;
    }
    if(needs_space && count_path_bytes(task, task->srcs[i], target, required)) {
      return -1;
    }
  }
  return 0;
}

static int count_reclaimable_bytes(file_task_t *task, const char *src,
                                   const char *dst,
                                   unsigned long long *reclaimable);

static int
count_reclaimable_dir_bytes(file_task_t *task, const char *src,
                            const char *dst,
                            unsigned long long *reclaimable) {
  DIR *dir = opendir(src);
  struct dirent *entry;
  int ret = -1;

  if(!dir) {
    return -1;
  }
  while((entry = readdir(dir))) {
    char src_child[PATH_MAX];
    char dst_child[PATH_MAX];

    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(task_cancel_requested(task)) {
      goto done;
    }
    if(path_join(src_child, sizeof(src_child), src, entry->d_name) ||
       path_join(dst_child, sizeof(dst_child), dst, entry->d_name) ||
       count_reclaimable_bytes(task, src_child, dst_child, reclaimable)) {
      goto done;
    }
  }
  ret = 0;
done:
  closedir(dir);
  return ret;
}

static int
count_reclaimable_bytes(file_task_t *task, const char *src, const char *dst,
                        unsigned long long *reclaimable) {
  struct stat src_st;
  struct stat dst_st;

  if(task_cancel_requested(task)) {
    return -1;
  }
  if(lstat(src, &src_st)) {
    return -1;
  }
  if(lstat(dst, &dst_st)) {
    if(errno == ENOENT) {
      return 0;
    }
    return -1;
  }
  if(S_ISREG(src_st.st_mode) && S_ISREG(dst_st.st_mode)) {
    *reclaimable += (unsigned long long)dst_st.st_size;
    return 0;
  }
  if(S_ISDIR(src_st.st_mode) && S_ISDIR(dst_st.st_mode)) {
    return count_reclaimable_dir_bytes(task, src, dst, reclaimable);
  }
  return 0;
}

static int
subtract_reclaimable_space(file_task_t *task, unsigned long long *required,
                           int copy_needed_only) {
  unsigned long long reclaimable = 0;
  size_t i;

  for(i = 0; i < task->src_count; i++) {
    char target[PATH_MAX];

    if(task_target_path(task, task->srcs[i], target, sizeof(target))) {
      return -1;
    }
    if(copy_needed_only) {
      int needs_space = move_requires_space_check(task->srcs[i], target);
      if(needs_space < 0) {
        return -1;
      }
      if(!needs_space) {
        continue;
      }
    }
    if(count_reclaimable_bytes(task, task->srcs[i], target, &reclaimable)) {
      return -1;
    }
  }

  *required = reclaimable >= *required ? 0 : *required - reclaimable;
  return 0;
}

static void *
task_worker(void *arg) {
  file_task_t *task = arg;
  unsigned long long total = 0;
  int ret = -1;

  task_update(task, TASK_RUNNING, "preparing", 0, NULL);

  if(task_cancel_requested(task)) {
    task_update(task, TASK_CANCELED, task->src, 0, "canceled");
    return NULL;
  }
  if(task->op == TASK_COPY || task->op == TASK_MOVE) {
    char error[160] = {0};
    char code[64] = {0};
    char arg[PATH_MAX] = {0};

    task_update(task, TASK_RUNNING, "checking target permissions", 0, NULL);
    if(check_task_targets_writable(task, error, sizeof(error),
                                   code, sizeof(code), arg, sizeof(arg))) {
      if(errno == ECANCELED || task_cancel_requested(task)) {
        task_update(task, TASK_CANCELED, task->current[0] ? task->current : task->src,
                    0, "canceled");
      } else {
        task_set_error_code(task, code, arg);
        task_update(task, TASK_FAILED, task->current[0] ? task->current : task->dst,
                    0, error[0] ? error : strerror(errno));
      }
      return NULL;
    }
  }
  if(task->op == TASK_COPY || task->op == TASK_MOVE || task->op == TASK_DELETE) {
    size_t i;
    for(i = 0; i < task->src_count; i++) {
      char target[PATH_MAX];
      const char *display = task->srcs[i];

      if((task->op == TASK_COPY || task->op == TASK_MOVE) &&
         task_target_path(task, task->srcs[i], target, sizeof(target))) {
        task_update(task, TASK_FAILED, task->srcs[i], 0, strerror(errno));
        return NULL;
      }
      if(task->op == TASK_COPY || task->op == TASK_MOVE) {
        display = target;
      }
      if(count_path_bytes(task, task->srcs[i], display, &total)) {
        if(errno == ECANCELED || task_cancel_requested(task)) {
          task_update(task, TASK_CANCELED, display, 0, "canceled");
        } else {
          task_update(task, TASK_FAILED, display, 0, strerror(errno));
        }
        return NULL;
      }
    }
    task_set_total(task, total);

    if(task->op == TASK_COPY || task->op == TASK_MOVE) {
      unsigned long long required = task->op == TASK_COPY ? total : 0;
      char error[128] = {0};
      char code[64] = {0};
      char arg[PATH_MAX] = {0};

      task_update(task, TASK_RUNNING, "checking target space", 0, NULL);
      if(task->op == TASK_MOVE && count_required_move_space(task, &required)) {
        if(errno == ECANCELED || task_cancel_requested(task)) {
          task_update(task, TASK_CANCELED, task->current[0] ? task->current : task->src,
                      0, "canceled");
        } else {
          task_update(task, TASK_FAILED, task->current[0] ? task->current : task->src,
                      0, strerror(errno));
        }
        return NULL;
      }
      if(required &&
         subtract_reclaimable_space(task, &required, task->op == TASK_MOVE)) {
        if(errno == ECANCELED || task_cancel_requested(task)) {
          task_update(task, TASK_CANCELED, task->current[0] ? task->current : task->src,
                      0, "canceled");
        } else {
          task_update(task, TASK_FAILED, task->current[0] ? task->current : task->src,
                      0, strerror(errno));
        }
        return NULL;
      }
      if(check_target_space(task->dst, required, error, sizeof(error),
                            code, sizeof(code), arg, sizeof(arg))) {
        task_set_error_code(task, code, arg);
        task_update(task, TASK_FAILED, task->dst, 0, error[0] ? error : strerror(errno));
        return NULL;
      }
    }
  }

  if(task->op == TASK_COPY) {
    size_t i;
    ret = 0;
    for(i = 0; i < task->src_count && !ret; i++) {
      char target[PATH_MAX];
      if(task_target_path(task, task->srcs[i], target, sizeof(target))) {
        ret = -1;
        break;
      }
      ret = copy_path(task, task->srcs[i], target);
    }
  } else if(task->op == TASK_MOVE) {
    size_t i;
    ret = 0;
    for(i = 0; i < task->src_count && !ret; i++) {
      char target[PATH_MAX];
      if(task_target_path(task, task->srcs[i], target, sizeof(target))) {
        ret = -1;
        break;
      }
      ret = move_path(task, task->srcs[i], target);
      if(!ret && task->src_count == 1) {
        task_update(task, TASK_RUNNING, target, total, NULL);
      }
    }
  } else if(task->op == TASK_DELETE) {
    size_t i;
    ret = 0;
    for(i = 0; i < task->src_count && !ret; i++) {
      ret = remove_path(task, task->srcs[i]);
    }
  }

  if(ret) {
    if(errno == ECANCELED || task_cancel_requested(task)) {
      task_update(task, TASK_CANCELED, task->current[0] ? task->current : task->src,
                  0, "canceled");
    } else {
      task_update(task, TASK_FAILED, task->current[0] ? task->current : task->src,
                  0, strerror(errno));
    }
  } else {
    pthread_mutex_lock(&g_tasks_lock);
    task->state = TASK_DONE;
    if(task->total) {
      task->done = task->total;
    }
    task->updated_at = time(NULL);
    pthread_mutex_unlock(&g_tasks_lock);
  }

  return NULL;
}

static enum MHD_Result
create_task_response(struct MHD_Connection *conn, task_op_t op,
                     char **srcs, size_t src_count,
                     const char *dst, int overwrite) {
  file_task_t *task = calloc(1, sizeof(file_task_t));
  strbuf_t b = {0};
  struct stat st;
  size_t i;

  if(!task) {
    free_paths(srcs, src_count);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
  }
  if(!src_count) {
    return task_request_error(conn, task, srcs, src_count,
                              MHD_HTTP_BAD_REQUEST, "no source paths");
  }
  if(dst && src_count > 1) {
    if(stat(dst, &st) || !S_ISDIR(st.st_mode)) {
      return task_request_error(conn, task, srcs, src_count,
                                MHD_HTTP_BAD_REQUEST,
                                "destination must be a directory for multiple items");
    }
  }
  if(dst) {
    for(i = 0; i < src_count; i++) {
      char target[PATH_MAX];
      char error[128] = {0};
      if(request_target_path(srcs[i], dst, src_count, target, sizeof(target))) {
        return task_request_error(conn, task, srcs, src_count,
                                  MHD_HTTP_BAD_REQUEST, NULL);
      }
      if(validate_task_target(srcs[i], target, overwrite, error, sizeof(error))) {
        return task_request_error(conn, task, srcs, src_count,
                                  MHD_HTTP_CONFLICT, error[0] ? error : NULL);
      }
    }
  }

  task->op = op;
  task->state = TASK_QUEUED;
  task->srcs = srcs;
  task->src_count = src_count;
  snprintf(task->src, sizeof(task->src), "%s%s",
           srcs[0], src_count > 1 ? " ..." : "");
  if(dst) {
    if(src_count == 1) {
      char target[PATH_MAX];
      if(request_target_path(srcs[0], dst, src_count, target, sizeof(target))) {
        return task_request_error(conn, task, srcs, src_count,
                                  MHD_HTTP_BAD_REQUEST, NULL);
      }
      snprintf(task->dst, sizeof(task->dst), "%s", target);
    } else {
      snprintf(task->dst, sizeof(task->dst), "%s", dst);
    }
  }
  task->created_at = time(NULL);
  task->updated_at = task->created_at;

  pthread_mutex_lock(&g_tasks_lock);
  remove_finished_tasks_locked();
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    return task_request_error(conn, task, srcs, src_count,
                              MHD_HTTP_CONFLICT, "another task is running");
  }
  task->id = g_next_task_id++;
  task->next = g_tasks;
  g_tasks = task;
  pthread_mutex_unlock(&g_tasks_lock);

  if(pthread_create(&task->thread, NULL, task_worker, task)) {
    task_update(task, TASK_FAILED, NULL, 0, "pthread_create failed");
  } else {
    pthread_detach(task->thread);
  }

  strbuf_printf(&b, "{\"ok\":true,\"task_id\":%lu}", task->id);
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

static enum MHD_Result
api_list(struct MHD_Connection *conn) {
  char *path = query_value(conn, "path");
  DIR *dir;
  struct dirent *entry;
  struct stat st;
  strbuf_t b = {0};
  int first = 1;

  if(has_active_task()) {
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }

  if(!path) {
    path = strdup("/");
  }
  if(!(dir = opendir(path))) {
    free(path);
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, NULL);
  }

  strbuf_append(&b, "{\"ok\":true,\"path\":");
  json_escape(&b, path);
  strbuf_append(&b, ",\"parent\":");
  char parent[PATH_MAX];
  if(path_dirname(path, parent, sizeof(parent))) {
    strcpy(parent, "/");
  }
  json_escape(&b, parent);
  strbuf_append(&b, ",\"entries\":[");

  while((entry = readdir(dir))) {
    char child[PATH_MAX];

    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(path_join(child, sizeof(child), path, entry->d_name) ||
       lstat(child, &st)) {
      continue;
    }

    if(!first) {
      strbuf_append(&b, ",");
    }
    first = 0;
    strbuf_append(&b, "{\"name\":");
    json_escape(&b, entry->d_name);
    strbuf_append(&b, ",\"path\":");
    json_escape(&b, child);
    strbuf_printf(&b, ",\"type\":\"%c\",\"mode\":%u,\"size\":%lld,\"mtime\":%lld}",
                  mode_type(&st), (unsigned int)(st.st_mode & 07777),
                  (long long)st.st_size, (long long)st.st_mtime);
  }

  closedir(dir);
  free(path);
  strbuf_append(&b, "]}");
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

static enum MHD_Result
api_tasks(struct MHD_Connection *conn) {
  strbuf_t b = {0};
  file_task_t *task;
  int first = 1;

  strbuf_append(&b, "{\"ok\":true,\"tasks\":[");
  pthread_mutex_lock(&g_tasks_lock);
  for(task = g_tasks; task; task = task->next) {
    if(!first) {
      strbuf_append(&b, ",");
    }
    first = 0;
    strbuf_printf(&b, "{\"id\":%lu,\"op\":\"%s\",\"state\":\"%s\",",
                  task->id, task_op_name(task->op), task_state_name(task->state));
    strbuf_append(&b, "\"src\":");
    json_escape(&b, task->src);
    strbuf_append(&b, ",\"dst\":");
    json_escape(&b, task->dst);
    strbuf_append(&b, ",\"current\":");
    json_escape(&b, task->current);
    strbuf_append(&b, ",\"error\":");
    json_escape(&b, task->error);
    strbuf_append(&b, ",\"error_code\":");
    json_escape(&b, task->error_code);
    strbuf_append(&b, ",\"error_arg\":");
    json_escape(&b, task->error_arg);
    strbuf_printf(&b, ",\"src_count\":%zu,\"total\":%llu,\"done\":%llu,\"speed\":%llu,\"cancel_requested\":%s,\"created_at\":%lld,\"elapsed\":%lld,\"updated_at\":%lld}",
                  task->src_count, task->total, task->done, task->speed,
                  task->cancel_requested ? "true" : "false",
                  (long long)task->created_at,
                  task->transfer_started_at ? (long long)(time(NULL) - task->transfer_started_at) : 0LL,
                  (long long)task->updated_at);
  }
  remove_finished_tasks_locked();
  pthread_mutex_unlock(&g_tasks_lock);
  strbuf_append(&b, "]}");
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

static enum MHD_Result
api_space(struct MHD_Connection *conn) {
  static const struct {
    const char *label_key;
    const char *path;
  } mounts[] = {
    {"storageInternal", "/data"},
    {"storageUsb", "/mnt/usb0"},
    {"storageUsb", "/mnt/usb1"},
    {"storageUsb", "/mnt/usb2"},
    {"storageUsb", "/mnt/usb3"},
    {"storageUsb", "/mnt/usb4"},
    {"storageUsb", "/mnt/usb5"},
    {"storageUsb", "/mnt/usb6"},
    {"storageUsb", "/mnt/usb7"},
    {"storageM2", "/mnt/ex1"},
    {"storageExtended", "/mnt/ext0"},
  };
  char *current = query_value(conn, "path");
  strbuf_t b = {0};
  int first = 1;

  strbuf_append(&b, "{\"ok\":true,\"spaces\":[");
  for(size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++) {
    struct statvfs vfs;
    struct stat st;
    unsigned long block_size;
    unsigned long long free_bytes;
    unsigned long long total_bytes;
    int is_current = 0;

    if(stat(mounts[i].path, &st) || !path_is_mounted(mounts[i].path, &st) ||
       statvfs(mounts[i].path, &vfs)) {
      continue;
    }
    block_size = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    free_bytes = vfs_bytes(vfs.f_bavail, block_size);
    total_bytes = vfs_bytes(vfs.f_blocks, block_size);
    if(current) {
      if(!strcmp(mounts[i].path, "/")) {
        is_current = !strcmp(current, "/");
      } else if(!strncmp(current, mounts[i].path, strlen(mounts[i].path)) &&
                (current[strlen(mounts[i].path)] == 0 ||
                 current[strlen(mounts[i].path)] == '/')) {
        is_current = 1;
      }
    }

    if(!first) {
      strbuf_append(&b, ",");
    }
    first = 0;
    strbuf_append(&b, "{\"label_key\":");
    json_escape(&b, mounts[i].label_key);
    strbuf_append(&b, ",\"path\":");
    json_escape(&b, mounts[i].path);
    strbuf_printf(&b, ",\"free\":%llu,\"total\":%llu,\"current\":%s}",
                  free_bytes, total_bytes, is_current ? "true" : "false");
  }
  free(current);
  strbuf_append(&b, "]}");
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

static enum MHD_Result
api_cancel(struct MHD_Connection *conn) {
  char *idstr = query_value(conn, "id");
  unsigned long id = idstr ? strtoul(idstr, NULL, 10) : 0;
  file_task_t *task;
  int found = 0;

  free(idstr);
  pthread_mutex_lock(&g_tasks_lock);
  for(task = g_tasks; task; task = task->next) {
    if(task->id == id && task_is_active(task)) {
      task->cancel_requested = 1;
      task->updated_at = time(NULL);
      found = 1;
      break;
    }
  }
  pthread_mutex_unlock(&g_tasks_lock);

  return found ? send_json_ok(conn) :
                 send_json_error(conn, MHD_HTTP_NOT_FOUND, "active task not found");
}

static void *
exit_later(void *arg) {
  (void)arg;
  usleep(250000);
  exit(0);
  return NULL;
}

static enum MHD_Result
api_exit(struct MHD_Connection *conn) {
  pthread_t thread;

  if(!pthread_create(&thread, NULL, exit_later, NULL)) {
    pthread_detach(thread);
  }
  return send_json_ok(conn);
}

static enum MHD_Result
api_copy(struct MHD_Connection *conn) {
  char *paths_raw = query_value(conn, "paths");
  char *dst = query_value(conn, "dst");
  char *overwrite = query_value(conn, "overwrite");
  char **paths = NULL;
  size_t count = 0;
  enum MHD_Result ret;

  if(!paths_raw || !dst || parse_paths(paths_raw, &paths, &count)) {
    free(paths_raw); free(dst); free(overwrite);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, NULL);
  }
  if(count == 1) {
    char target[PATH_MAX];
    if(request_target_path(paths[0], dst, count, target, sizeof(target)) ||
       !strcmp(paths[0], target)) {
      free_paths(paths, count); free(paths_raw); free(dst); free(overwrite);
      return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "source and destination are the same");
    }
  }
  ret = create_task_response(conn, TASK_COPY, paths, count, dst,
                             overwrite && !strcmp(overwrite, "1"));
  free(paths_raw); free(dst); free(overwrite);
  return ret;
}

static enum MHD_Result
api_move(struct MHD_Connection *conn) {
  char *paths_raw = query_value(conn, "paths");
  char *dst = query_value(conn, "dst");
  char *overwrite = query_value(conn, "overwrite");
  char **paths = NULL;
  size_t count = 0;
  enum MHD_Result ret;

  if(!paths_raw || !dst || parse_paths(paths_raw, &paths, &count)) {
    free(paths_raw); free(dst); free(overwrite);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, NULL);
  }
  ret = create_task_response(conn, TASK_MOVE, paths, count, dst,
                             overwrite && !strcmp(overwrite, "1"));
  free(paths_raw); free(dst); free(overwrite);
  return ret;
}

static enum MHD_Result
api_delete(struct MHD_Connection *conn) {
  char *paths_raw = query_value(conn, "paths");
  char **paths = NULL;
  size_t count = 0;
  enum MHD_Result ret;

  if(!paths_raw || parse_paths(paths_raw, &paths, &count)) {
    free(paths_raw);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  for(size_t i = 0; i < count; i++) {
    if(!strcmp(paths[i], "/")) {
      free_paths(paths, count); free(paths_raw);
      return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
    }
  }
  ret = create_task_response(conn, TASK_DELETE, paths, count, NULL, 0);
  free(paths_raw);
  return ret;
}

static enum MHD_Result
api_rename(struct MHD_Connection *conn) {
  char *path = query_value(conn, "path");
  char *name = query_value(conn, "name");
  char parent[PATH_MAX];
  char target[PATH_MAX];
  int ret;

  pthread_mutex_lock(&g_tasks_lock);
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    free(path); free(name);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  pthread_mutex_unlock(&g_tasks_lock);

  if(!path || !name || path_dirname(path, parent, sizeof(parent)) ||
     path_join(target, sizeof(target), parent, name)) {
    free(path); free(name);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, NULL);
  }

  ret = rename(path, target);
  free(path); free(name);
  return ret ? send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, NULL)
             : send_json_ok(conn);
}

static enum MHD_Result
api_mkdir(struct MHD_Connection *conn) {
  char *path = query_value(conn, "path");
  char *name = query_value(conn, "name");
  char target[PATH_MAX];
  int ret;

  pthread_mutex_lock(&g_tasks_lock);
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    free(path); free(name);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  pthread_mutex_unlock(&g_tasks_lock);

  if(!path || !name || path_join(target, sizeof(target), path, name)) {
    free(path); free(name);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, NULL);
  }
  ret = mkdir(target, 0777);
  if(!ret) {
    ret = chmod_path_0777(target);
  }
  free(path); free(name);
  return ret ? send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, NULL)
             : send_json_ok(conn);
}

enum MHD_Result
filemgr_api_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/api/list")) return api_list(conn);
  if(!strcmp(url, "/api/tasks")) return api_tasks(conn);
  if(!strcmp(url, "/api/space")) return api_space(conn);
  if(!strcmp(url, "/api/cancel")) return api_cancel(conn);
  if(!strcmp(url, "/api/exit")) return api_exit(conn);
  if(!strcmp(url, "/api/copy")) return api_copy(conn);
  if(!strcmp(url, "/api/move")) return api_move(conn);
  if(!strcmp(url, "/api/delete")) return api_delete(conn);
  if(!strcmp(url, "/api/rename")) return api_rename(conn);
  if(!strcmp(url, "/api/mkdir")) return api_mkdir(conn);
  return send_json_error(conn, MHD_HTTP_NOT_FOUND, "unknown api");
}

static ssize_t
file_read(void *cls, uint64_t pos, char *buf, size_t max) {
  FILE *file = cls;
  size_t len;

  if(fseek(file, (long)pos, SEEK_SET)) {
    return MHD_CONTENT_READER_END_WITH_ERROR;
  }
  if(!(len = fread(buf, 1, max, file))) {
    return ferror(file) ? MHD_CONTENT_READER_END_WITH_ERROR :
                          MHD_CONTENT_READER_END_OF_STREAM;
  }
  return (ssize_t)len;
}

static void
file_close(void *cls) {
  fclose((FILE *)cls);
}

enum MHD_Result
filemgr_fs_request(struct MHD_Connection *conn) {
  const char *path = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "path");
  struct MHD_Response *resp;
  enum MHD_Result ret = MHD_NO;
  struct stat st;
  FILE *file;

  if(has_active_task()) {
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }

  if(!path || stat(path, &st) || !S_ISREG(st.st_mode) ||
     !(file = fopen(path, "rb"))) {
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "file not found");
  }

  if((resp = MHD_create_response_from_callback((uint64_t)st.st_size,
                                               32 * 0x4000, file_read, file,
                                               file_close))) {
    const char *mime = mime_get_type(path);
    if(mime) {
      MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
    }
    ret = websrv_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return ret;
  }

  fclose(file);
  return MHD_NO;
}
