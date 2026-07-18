#include "filemgr.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
#define COPY_PIPELINE_SLOTS 3
#define FILEMGR_AGGRESSIVE_COPY 0
#define FILEMGR_PIPELINE_COPY 1
#define SMALL_COPY_WORKERS 3
#define FILE_TASK_QUEUE_LIMIT 128
#define LARGE_FILE_THRESHOLD (256LL * 1024 * 1024)
#define TEXT_FILE_MAX_SIZE (1024 * 1024)
#define TRANSFER_ALERT_THRESHOLD (10 * 60)
#define ETA_AVERAGE_WINDOW_SECONDS 30
#define ETA_SAMPLE_SLOTS 64
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

typedef struct task_eta_sample {
  unsigned long long done;
  struct timespec time;
} task_eta_sample_t;

typedef struct file_task {
  unsigned long id;
  task_op_t op;
  task_state_t state;
  char src[PATH_MAX];
  char dst[PATH_MAX];
  char current[PATH_MAX];
  char error[160];
  char error_code[64];
  char error_arg[PATH_MAX + 96];
  char **srcs;
  size_t src_count;
  size_t file_count;
  size_t dir_count;
  unsigned long long total;
  unsigned long long done;
  unsigned long long speed;
  unsigned long long eta;
  unsigned long long speed_sample_done;
  struct timespec speed_sample_time;
  task_eta_sample_t eta_samples[ETA_SAMPLE_SLOTS];
  unsigned int eta_sample_next;
  unsigned int eta_sample_count;
  int cancel_requested;
  time_t created_at;
  time_t transfer_started_at;
  time_t updated_at;
  pthread_t thread;
  struct file_task *next;
} file_task_t;

typedef struct task_completion {
  unsigned long id;
  task_op_t op;
  char src[PATH_MAX];
  size_t src_count;
  unsigned long long total;
  size_t file_count;
  time_t elapsed;
} task_completion_t;

static char *fs_path_value(char *path);
#if FILEMGR_PIPELINE_COPY
typedef struct copy_pipeline_slot {
  char *data;
  size_t size;
  int ready;
} copy_pipeline_slot_t;

typedef struct copy_pipeline {
  file_task_t *task;
  int in;
  copy_pipeline_slot_t slots[COPY_PIPELINE_SLOTS];
  pthread_mutex_t lock;
  pthread_cond_t can_read;
  pthread_cond_t can_write;
  int read_index;
  int write_index;
  int done;
  int error;
  int error_number;
} copy_pipeline_t;
#endif

#if FILEMGR_AGGRESSIVE_COPY
typedef struct copy_job {
  char src[PATH_MAX];
  char dst[PATH_MAX];
  struct copy_job *next;
} copy_job_t;

typedef struct copy_queue {
  file_task_t *task;
  pthread_mutex_t lock;
  pthread_cond_t has_work;
  pthread_cond_t has_space;
  pthread_cond_t idle;
  pthread_t workers[SMALL_COPY_WORKERS];
  copy_job_t *head;
  copy_job_t *tail;
  int queued;
  int active;
  int stopping;
  int error;
  int error_number;
  int worker_count;
} copy_queue_t;
#endif

static pthread_mutex_t g_tasks_lock = PTHREAD_MUTEX_INITIALIZER;
static file_task_t *g_tasks = NULL;
static unsigned long g_next_task_id = 1;
static task_completion_t g_last_completion;

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
  char *value = raw ? strdup(raw) : NULL;

  if(value && !strcmp(key, "name")) {
    char *start = value;
    char *end;
    while(isspace((unsigned char)*start)) start++;
    end = start + strlen(start);
    while(end > start && isspace((unsigned char)end[-1])) end--;
    memmove(value, start, (size_t)(end - start));
    value[end - start] = 0;
  }
  return value;
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
    if(!(paths[count] = fs_path_value(strdup(line)))) {
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

static long long
timespec_delta_ns(const struct timespec *end, const struct timespec *start) {
  return (long long)(end->tv_sec - start->tv_sec) * 1000000000LL +
         (long long)(end->tv_nsec - start->tv_nsec);
}

static void
task_update_eta_locked(file_task_t *task, const struct timespec *now_mono) {
  task_eta_sample_t *sample;
  task_eta_sample_t *base = NULL;
  unsigned int i;

  if(!task->total || !task->done || task->done >= task->total) {
    task->eta = 0;
    return;
  }

  sample = &task->eta_samples[task->eta_sample_next];
  sample->done = task->done;
  sample->time = *now_mono;
  task->eta_sample_next = (task->eta_sample_next + 1) % ETA_SAMPLE_SLOTS;
  if(task->eta_sample_count < ETA_SAMPLE_SLOTS) {
    task->eta_sample_count++;
  }

  for(i = 0; i < task->eta_sample_count; i++) {
    task_eta_sample_t *candidate = &task->eta_samples[i];
    long long age_ns;

    if(!candidate->time.tv_sec || candidate->done >= task->done) {
      continue;
    }
    age_ns = timespec_delta_ns(now_mono, &candidate->time);
    if(age_ns <= 0 || age_ns > (long long)ETA_AVERAGE_WINDOW_SECONDS * 1000000000LL) {
      continue;
    }
    if(!base || age_ns > timespec_delta_ns(now_mono, &base->time)) {
      base = candidate;
    }
  }

  if(base) {
    long long elapsed_ns = timespec_delta_ns(now_mono, &base->time);
    unsigned long long delta = task->done - base->done;
    unsigned long long remaining = task->total - task->done;
    if(delta && elapsed_ns > 0) {
      long double seconds = (long double)elapsed_ns / 1000000000.0L;
      long double eta = ((long double)remaining / (long double)delta) * seconds;
      task->eta = eta > 0 ? (unsigned long long)(eta + 0.999999L) : 0;
      return;
    }
  }

  task->eta = task->speed ? (task->total - task->done + task->speed - 1) / task->speed : 0;
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
      long long elapsed_ns = timespec_delta_ns(&now_mono, &task->speed_sample_time);
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
    task_update_eta_locked(task, &now_mono);
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
utf8_decode_char(const char **ps, unsigned int *out) {
  const unsigned char *s = (const unsigned char *)*ps;
  unsigned char c = s[0];
  unsigned int cp;
  size_t n;
  size_t i;

  if(c < 0x80) {
    *out = c;
    *ps += 1;
    return 0;
  }
  if((c & 0xe0) == 0xc0) {
    cp = c & 0x1f;
    n = 2;
  } else if((c & 0xf0) == 0xe0) {
    cp = c & 0x0f;
    n = 3;
  } else if((c & 0xf8) == 0xf0) {
    cp = c & 0x07;
    n = 4;
  } else {
    return -1;
  }

  for(i = 1; i < n; i++) {
    unsigned char t = s[i];
    if(!t || (t & 0xc0) != 0x80) {
      return -1;
    }
    cp = (cp << 6) | (t & 0x3f);
  }
  if((n == 2 && cp < 0x80) ||
     (n == 3 && cp < 0x800) ||
     (n == 4 && (cp < 0x10000 || cp > 0x10ffff)) ||
     (cp >= 0xd800 && cp <= 0xdfff)) {
    return -1;
  }

  *out = cp;
  *ps += n;
  return 0;
}

static char *
fs_path_value(char *path) {
  const char *p;
  char *out;
  size_t len;
  size_t pos = 0;
  int has_byte_token = 0;

  if(!path) {
    return path;
  }
  for(p = path; *p;) {
    unsigned int cp;
    const char *next = p;

    if(utf8_decode_char(&next, &cp)) {
      return path;
    }
    if(cp >= 0x80 && cp <= 0xff) {
      has_byte_token = 1;
    } else if(cp > 0xff) {
      return path;
    }
    p = next;
  }
  if(!has_byte_token) {
    return path;
  }

  len = strlen(path);
  if(!(out = malloc(len + 1))) {
    return path;
  }
  for(p = path; *p;) {
    unsigned int cp;
    const char *next = p;

    if(utf8_decode_char(&next, &cp)) {
      free(out);
      return path;
    }
    out[pos++] = (char)(unsigned char)cp;
    p = next;
  }
  out[pos] = 0;
  free(path);
  return out;
}

static int
json_escape(strbuf_t *b, const char *s) {
  if(strbuf_append(b, "\"")) return -1;
  while(*s) {
    unsigned char c = (unsigned char)*s;
    switch(c) {
    case '"': if(strbuf_append(b, "\\\"")) return -1; s++; break;
    case '\\': if(strbuf_append(b, "\\\\")) return -1; s++; break;
    case '\b': if(strbuf_append(b, "\\b")) return -1; s++; break;
    case '\f': if(strbuf_append(b, "\\f")) return -1; s++; break;
    case '\n': if(strbuf_append(b, "\\n")) return -1; s++; break;
    case '\r': if(strbuf_append(b, "\\r")) return -1; s++; break;
    case '\t': if(strbuf_append(b, "\\t")) return -1; s++; break;
    default:
      if(c < 0x20 || c >= 0x80) {
        if(strbuf_printf(b, "\\u%04x", c)) return -1;
      } else {
        if(strbuf_reserve(b, 1)) return -1;
        b->data[b->len++] = (char)c;
        b->data[b->len] = 0;
      }
      s++;
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

static enum MHD_Result
send_json_version(struct MHD_Connection *conn, unsigned long long version) {
  char *data;

  if(asprintf(&data, "{\"ok\":true,\"version\":\"%016llx\"}", version) < 0) {
    return MHD_NO;
  }
  return send_buffer(conn, MHD_HTTP_OK, data, "application/json");
}

static const char *
api_error_code(const char *msg) {
  if(!msg) return "system_error";
  if(!strcmp(msg, "another task is running")) return "active_task";
  if(!strcmp(msg, "active task not found")) return "active_task_not_found";
  if(!strcmp(msg, "source and destination are the same")) return "source_destination_same";
  if(!strcmp(msg, "destination is inside source directory")) return "destination_inside_source";
  if(!strcmp(msg, "invalid path")) return "invalid_path";
  if(!strcmp(msg, "file not found")) return "file_not_found";
  if(!strcmp(msg, "invalid method")) return "invalid_method";
  if(!strcmp(msg, "unknown api")) return "unknown_api";
  if(!strcmp(msg, "out of memory")) return "out_of_memory";
  if(!strcmp(msg, "no source paths")) return "no_source_paths";
  if(!strcmp(msg, "file type is not editable")) return "text_type_not_editable";
  if(!strcmp(msg, "text file is too large")) return "text_file_too_large";
  if(!strcmp(msg, "file is not valid UTF-8")) return "text_invalid_utf8";
  if(!strcmp(msg, "file changed since it was opened")) return "text_file_changed";
  if(!strcmp(msg, "text file is not writable")) return "text_file_not_writable";
  if(!strcmp(msg, "file already exists")) return "file_already_exists";
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

static int
text_extension_allowed(const char *path) {
  static const char *extensions[] = {
    ".txt", ".json", ".xml", ".ini", ".cfg", ".conf", ".md",
    ".log", ".lua", ".js", ".css", ".html", ".htm", ".c", ".h",
    ".cpp", ".hpp", ".sh", ".csv", ".yaml", ".yml"
  };
  const char *extension = strrchr(path, '.');
  size_t i;

  if(!extension) {
    return 0;
  }
  for(i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
    if(!strcasecmp(extension, extensions[i])) {
      return 1;
    }
  }
  return 0;
}

static int
valid_utf8(const unsigned char *data, size_t size) {
  size_t i = 0;

  while(i < size) {
    unsigned char c = data[i++];
    size_t trailing;
    unsigned int codepoint;

    if(!c) return 0;
    if(c < 0x80) continue;
    if(c >= 0xc2 && c <= 0xdf) {
      trailing = 1;
      codepoint = c & 0x1f;
    } else if(c >= 0xe0 && c <= 0xef) {
      trailing = 2;
      codepoint = c & 0x0f;
    } else if(c >= 0xf0 && c <= 0xf4) {
      trailing = 3;
      codepoint = c & 0x07;
    } else {
      return 0;
    }
    if(trailing > size - i) return 0;
    while(trailing--) {
      unsigned char next = data[i++];
      if((next & 0xc0) != 0x80) return 0;
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if((codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff ||
       (codepoint < 0x800 && c >= 0xe0) ||
       (codepoint < 0x10000 && c >= 0xf0)) {
      return 0;
    }
  }
  return 1;
}

static unsigned long long
text_version(const unsigned char *data, size_t size) {
  unsigned long long hash = 1469598103934665603ULL;
  size_t i;

  for(i = 0; i < size; i++) {
    hash ^= data[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

typedef enum text_newline {
  TEXT_NEWLINE_LF,
  TEXT_NEWLINE_CRLF,
  TEXT_NEWLINE_CR,
} text_newline_t;

static text_newline_t
detect_text_newline(const unsigned char *data, size_t size) {
  size_t crlf = 0;
  size_t lf = 0;
  size_t cr = 0;
  size_t i;

  for(i = 0; i < size; i++) {
    if(data[i] == '\r') {
      if(i + 1 < size && data[i + 1] == '\n') {
        crlf++;
        i++;
      } else {
        cr++;
      }
    } else if(data[i] == '\n') {
      lf++;
    }
  }
  if(crlf > lf && crlf >= cr) return TEXT_NEWLINE_CRLF;
  if(cr > lf && cr > crlf) return TEXT_NEWLINE_CR;
  return TEXT_NEWLINE_LF;
}

static int
format_text_for_save(const char *body, size_t body_size,
                     const unsigned char *current, size_t current_size,
                     char **output, size_t *output_size) {
  static const unsigned char bom[] = {0xef, 0xbb, 0xbf};
  int keep_bom = current_size >= sizeof(bom) &&
                 !memcmp(current, bom, sizeof(bom));
  text_newline_t newline = detect_text_newline(
    current + (keep_bom ? sizeof(bom) : 0),
    current_size - (keep_bom ? sizeof(bom) : 0));
  const unsigned char *input = (const unsigned char *)(body ? body : "");
  size_t input_size = body_size;
  size_t capacity = body_size * (newline == TEXT_NEWLINE_CRLF ? 2 : 1) +
                    sizeof(bom) + 1;
  char *formatted;
  size_t i;
  size_t len = 0;

  if(input_size >= sizeof(bom) && !memcmp(input, bom, sizeof(bom))) {
    input += sizeof(bom);
    input_size -= sizeof(bom);
  }
  if(!(formatted = malloc(capacity))) {
    errno = ENOMEM;
    return -1;
  }
  if(keep_bom) {
    memcpy(formatted + len, bom, sizeof(bom));
    len += sizeof(bom);
  }
  for(i = 0; i < input_size; i++) {
    unsigned char c = input[i];

    if(c != '\r' && c != '\n') {
      formatted[len++] = (char)c;
      continue;
    }
    if(c == '\r' && i + 1 < input_size && input[i + 1] == '\n') {
      i++;
    }
    if(newline == TEXT_NEWLINE_CRLF) {
      formatted[len++] = '\r';
      formatted[len++] = '\n';
    } else {
      formatted[len++] = newline == TEXT_NEWLINE_CR ? '\r' : '\n';
    }
  }
  if(len > TEXT_FILE_MAX_SIZE) {
    free(formatted);
    errno = EFBIG;
    return -1;
  }
  formatted[len] = 0;
  *output = formatted;
  *output_size = len;
  return 0;
}

static int
read_text_file(const char *path, char **data, size_t *size,
               struct stat *st) {
  FILE *file;
  size_t read_size;

  *data = NULL;
  *size = 0;
  if(lstat(path, st) || !S_ISREG(st->st_mode)) {
    return -1;
  }
  if(st->st_size < 0 || (unsigned long long)st->st_size > TEXT_FILE_MAX_SIZE) {
    errno = EFBIG;
    return -1;
  }
  if(!(file = fopen(path, "rb"))) {
    return -1;
  }
  if(!(*data = malloc((size_t)st->st_size + 1))) {
    fclose(file);
    errno = ENOMEM;
    return -1;
  }
  read_size = fread(*data, 1, (size_t)st->st_size, file);
  if(read_size != (size_t)st->st_size || ferror(file)) {
    free(*data);
    *data = NULL;
    fclose(file);
    return -1;
  }
  fclose(file);
  (*data)[read_size] = 0;
  *size = read_size;
  return 0;
}

static enum MHD_Result
send_text_file(struct MHD_Connection *conn, char *data, size_t size,
               unsigned long long version) {
  struct MHD_Response *resp;
  enum MHD_Result ret;
  char version_text[24];

  if(!(resp = MHD_create_response_from_buffer(size, data,
                                               MHD_RESPMEM_MUST_FREE))) {
    free(data);
    return MHD_NO;
  }
  snprintf(version_text, sizeof(version_text), "%016llx", version);
  MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
                          "text/plain; charset=utf-8");
  MHD_add_response_header(resp, "X-Text-Version", version_text);
  ret = websrv_queue_response(conn, MHD_HTTP_OK, resp);
  MHD_destroy_response(resp);
  return ret;
}

static int task_target_path(file_task_t *task, const char *src,
                            char *out, size_t size);

static int count_path_bytes_sync(file_task_t *task, const char *path,
                                 const char *display, const char *target,
                                 unsigned long long *total,
                                 unsigned long long *reclaimable,
                                 size_t *file_count, size_t *dir_count);

static int
count_dir_bytes_sync(file_task_t *task, const char *path, const char *display,
                     const char *target, unsigned long long *total,
                     unsigned long long *reclaimable, size_t *file_count,
                     size_t *dir_count) {
  DIR *dir = opendir(path);
  struct dirent *entry;
  int ret = -1;

  if(!dir) {
    return -1;
  }
  while((entry = readdir(dir))) {
    char child[PATH_MAX];
    char display_child[PATH_MAX];
    char target_child[PATH_MAX];
    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(task && task_cancel_requested(task)) {
      goto done;
    }
    if(path_join(child, sizeof(child), path, entry->d_name) ||
       (display && path_join(display_child, sizeof(display_child), display,
                             entry->d_name)) ||
       (target && path_join(target_child, sizeof(target_child), target,
                            entry->d_name)) ||
       count_path_bytes_sync(task, child, display ? display_child : NULL,
                             target ? target_child : NULL, total, reclaimable,
                             file_count, dir_count)) {
      goto done;
    }
  }
  ret = 0;
done:
  closedir(dir);
  return ret;
}

static int
count_path_bytes_sync(file_task_t *task, const char *path, const char *display,
                      const char *target, unsigned long long *total,
                      unsigned long long *reclaimable, size_t *file_count,
                      size_t *dir_count) {
  struct stat st;
  struct stat target_st;
  int target_exists = 0;

  if(task && task_cancel_requested(task)) {
    return -1;
  }
  if(task) {
    task_update(task, TASK_RUNNING, display ? display : path, 0, NULL);
  }
  if(lstat(path, &st)) {
    return -1;
  }
  if(target) {
    if(!lstat(target, &target_st)) {
      target_exists = 1;
    } else if(errno != ENOENT) {
      return -1;
    }
  }
  if(S_ISDIR(st.st_mode)) {
    if(dir_count) (*dir_count)++;
    return count_dir_bytes_sync(task, path, display,
                                target_exists && S_ISDIR(target_st.st_mode) ?
                                target : NULL,
                                total, reclaimable, file_count, dir_count);
  }
  if(S_ISREG(st.st_mode)) {
    *total += (unsigned long long)st.st_size;
    if(reclaimable && target_exists && S_ISREG(target_st.st_mode)) {
      *reclaimable += (unsigned long long)target_st.st_size;
    }
    if(file_count) (*file_count)++;
  }
  return 0;
}

#if FILEMGR_AGGRESSIVE_COPY
typedef struct count_job {
  char path[PATH_MAX];
  char display[PATH_MAX];
  char target[PATH_MAX];
  int has_display;
  int has_target;
  struct count_job *next;
} count_job_t;

typedef struct count_queue {
  file_task_t *task;
  pthread_mutex_t lock;
  pthread_cond_t has_work;
  pthread_cond_t has_space;
  pthread_cond_t idle;
  pthread_t workers[SMALL_COPY_WORKERS];
  count_job_t *head;
  count_job_t *tail;
  int queued;
  int active;
  int stopping;
  int error;
  int error_number;
  int worker_count;
  unsigned long long total;
  unsigned long long reclaimable;
  size_t file_count;
  size_t dir_count;
} count_queue_t;

static void
count_queue_add(count_queue_t *queue, unsigned long long total,
                unsigned long long reclaimable, size_t file_count,
                size_t dir_count) {
  pthread_mutex_lock(&queue->lock);
  queue->total += total;
  queue->reclaimable += reclaimable;
  queue->file_count += file_count;
  queue->dir_count += dir_count;
  pthread_mutex_unlock(&queue->lock);
}

static void *
count_queue_worker(void *arg) {
  count_queue_t *queue = arg;

  for(;;) {
    count_job_t *job;
    unsigned long long total = 0;
    unsigned long long reclaimable = 0;
    size_t file_count = 0;
    size_t dir_count = 0;
    int ret;

    pthread_mutex_lock(&queue->lock);
    while(!queue->stopping && !queue->head) {
      pthread_cond_wait(&queue->has_work, &queue->lock);
    }
    if(queue->stopping && !queue->head) {
      pthread_mutex_unlock(&queue->lock);
      return NULL;
    }
    job = queue->head;
    queue->head = job->next;
    if(!queue->head) {
      queue->tail = NULL;
    }
    queue->queued--;
    queue->active++;
    pthread_cond_signal(&queue->has_space);
    pthread_mutex_unlock(&queue->lock);

    ret = count_path_bytes_sync(queue->task, job->path,
                                job->has_display ? job->display : NULL,
                                job->has_target ? job->target : NULL,
                                &total, &reclaimable, &file_count, &dir_count);
    if(!ret) {
      count_queue_add(queue, total, reclaimable, file_count, dir_count);
    }

    pthread_mutex_lock(&queue->lock);
    if(ret) {
      queue->error = 1;
      queue->error_number = errno ? errno : EIO;
      queue->stopping = 1;
      pthread_cond_broadcast(&queue->has_work);
    }
    queue->active--;
    if(!queue->head && !queue->active) {
      pthread_cond_broadcast(&queue->idle);
    }
    pthread_mutex_unlock(&queue->lock);
    free(job);
  }
}

static int
count_queue_init(count_queue_t *queue, file_task_t *task) {
  int i;

  memset(queue, 0, sizeof(*queue));
  queue->task = task;
  if(pthread_mutex_init(&queue->lock, NULL)) {
    return -1;
  }
  if(pthread_cond_init(&queue->has_work, NULL)) {
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  if(pthread_cond_init(&queue->has_space, NULL)) {
    pthread_cond_destroy(&queue->has_work);
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  if(pthread_cond_init(&queue->idle, NULL)) {
    pthread_cond_destroy(&queue->has_space);
    pthread_cond_destroy(&queue->has_work);
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  for(i = 0; i < SMALL_COPY_WORKERS; i++) {
    if(pthread_create(&queue->workers[i], NULL, count_queue_worker, queue)) {
      queue->stopping = 1;
      pthread_cond_broadcast(&queue->has_work);
      while(queue->worker_count > 0) {
        pthread_join(queue->workers[--queue->worker_count], NULL);
      }
      pthread_cond_destroy(&queue->idle);
      pthread_cond_destroy(&queue->has_space);
      pthread_cond_destroy(&queue->has_work);
      pthread_mutex_destroy(&queue->lock);
      return -1;
    }
    queue->worker_count++;
  }
  return 0;
}

static int
count_queue_enqueue(count_queue_t *queue, const char *path, const char *display,
                    const char *target) {
  count_job_t *job;

  if(!(job = calloc(1, sizeof(*job)))) {
    return -1;
  }
  snprintf(job->path, sizeof(job->path), "%s", path);
  if(display) {
    snprintf(job->display, sizeof(job->display), "%s", display);
    job->has_display = 1;
  }
  if(target) {
    snprintf(job->target, sizeof(job->target), "%s", target);
    job->has_target = 1;
  }

  pthread_mutex_lock(&queue->lock);
  while(!queue->stopping && queue->queued >= FILE_TASK_QUEUE_LIMIT) {
    pthread_cond_wait(&queue->has_space, &queue->lock);
  }
  if(queue->stopping || queue->error || task_cancel_requested(queue->task)) {
    pthread_mutex_unlock(&queue->lock);
    free(job);
    errno = queue->error_number ? queue->error_number : ECANCELED;
    return -1;
  }
  if(queue->tail) {
    queue->tail->next = job;
  } else {
    queue->head = job;
  }
  queue->tail = job;
  queue->queued++;
  pthread_cond_signal(&queue->has_work);
  pthread_mutex_unlock(&queue->lock);
  return 0;
}

static int
count_queue_finish(count_queue_t *queue, int abort_pending) {
  count_job_t *job;
  int ret = abort_pending ? -1 : 0;
  int i;

  pthread_mutex_lock(&queue->lock);
  while(!abort_pending && !queue->error && (queue->head || queue->active)) {
    pthread_cond_wait(&queue->idle, &queue->lock);
  }
  if(queue->error) {
    errno = queue->error_number ? queue->error_number : EIO;
    ret = -1;
  }
  queue->stopping = 1;
  if(abort_pending || ret) {
    while(queue->head) {
      job = queue->head;
      queue->head = job->next;
      free(job);
    }
    queue->tail = NULL;
    queue->queued = 0;
  }
  pthread_cond_broadcast(&queue->has_work);
  pthread_cond_broadcast(&queue->has_space);
  pthread_mutex_unlock(&queue->lock);

  for(i = 0; i < queue->worker_count; i++) {
    pthread_join(queue->workers[i], NULL);
  }
  while(queue->head) {
    job = queue->head;
    queue->head = job->next;
    free(job);
  }
  pthread_cond_destroy(&queue->idle);
  pthread_cond_destroy(&queue->has_space);
  pthread_cond_destroy(&queue->has_work);
  pthread_mutex_destroy(&queue->lock);
  return ret;
}

static int
count_path_bytes(file_task_t *task, const char *path, const char *display,
                 const char *target, unsigned long long *total,
                 unsigned long long *reclaimable, size_t *file_count,
                 size_t *dir_count) {
  DIR *dir;
  struct dirent *entry;
  struct stat st;
  struct stat target_st;
  int target_exists = 0;
  int ret = -1;
  int queue_finished = 0;
  count_queue_t queue;

  if(task && task_cancel_requested(task)) {
    return -1;
  }
  if(lstat(path, &st)) {
    return -1;
  }
  if(!S_ISDIR(st.st_mode)) {
    return count_path_bytes_sync(task, path, display, target, total,
                                 reclaimable, file_count, dir_count);
  }
  if(task) {
    task_update(task, TASK_RUNNING, display ? display : path, 0, NULL);
  }
  if(target) {
    if(!lstat(target, &target_st)) {
      target_exists = S_ISDIR(target_st.st_mode);
    } else if(errno != ENOENT) {
      return -1;
    }
  }
  if(dir_count) (*dir_count)++;
  if(count_queue_init(&queue, task)) {
    return -1;
  }
  if(!(dir = opendir(path))) {
    count_queue_finish(&queue, 1);
    return -1;
  }

  while((entry = readdir(dir))) {
    char child[PATH_MAX];
    char display_child[PATH_MAX];
    char target_child[PATH_MAX];

    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(task && task_cancel_requested(task)) {
      errno = ECANCELED;
      goto done;
    }
    if(path_join(child, sizeof(child), path, entry->d_name) ||
       (display && path_join(display_child, sizeof(display_child), display,
                             entry->d_name)) ||
       (target_exists && path_join(target_child, sizeof(target_child), target,
                                   entry->d_name)) ||
       count_queue_enqueue(&queue, child, display ? display_child : NULL,
                           target_exists ? target_child : NULL)) {
      goto done;
    }
  }

  ret = count_queue_finish(&queue, 0);
  queue_finished = 1;
  if(!ret) {
    *total += queue.total;
    if(reclaimable) {
      *reclaimable += queue.reclaimable;
    }
    if(file_count) {
      *file_count += queue.file_count;
    }
    if(dir_count) {
      *dir_count += queue.dir_count;
    }
  }
done:
  closedir(dir);
  if(ret && !queue_finished) {
    count_queue_finish(&queue, 1);
  }
  return ret;
}
#else
#define count_path_bytes count_path_bytes_sync
#endif

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
copy_file_buffered(file_task_t *task, const char *src, const char *dst) {
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

#if FILEMGR_PIPELINE_COPY
static void
pipeline_fail_locked(copy_pipeline_t *p, int error) {
  p->error = 1;
  p->done = 1;
  p->error_number = error ? error : EIO;
  pthread_cond_broadcast(&p->can_read);
  pthread_cond_broadcast(&p->can_write);
}

static void *
copy_pipeline_reader(void *arg) {
  copy_pipeline_t *p = arg;

  for(;;) {
    int slot;
    ssize_t n;

    pthread_mutex_lock(&p->lock);
    while(!p->error && !p->done && p->slots[p->read_index].ready) {
      pthread_cond_wait(&p->can_read, &p->lock);
    }
    if(p->error || p->done || task_cancel_requested(p->task)) {
      p->done = 1;
      pthread_cond_broadcast(&p->can_write);
      pthread_mutex_unlock(&p->lock);
      return NULL;
    }
    slot = p->read_index;
    p->read_index = (p->read_index + 1) % COPY_PIPELINE_SLOTS;
    pthread_mutex_unlock(&p->lock);

    n = read(p->in, p->slots[slot].data, COPY_BUFFER_SIZE);

    pthread_mutex_lock(&p->lock);
    if(n < 0) {
      pipeline_fail_locked(p, errno);
    } else if(n == 0) {
      p->done = 1;
      pthread_cond_broadcast(&p->can_write);
    } else {
      p->slots[slot].size = (size_t)n;
      p->slots[slot].ready = 1;
      pthread_cond_signal(&p->can_write);
    }
    pthread_mutex_unlock(&p->lock);
  }
}

static int
copy_file_pipeline(file_task_t *task, const char *src, const char *dst) {
  copy_pipeline_t p;
  pthread_t reader;
  int out = -1;
  int ret = -1;
  int reader_started = 0;
  int lock_ready = 0;
  int can_read_ready = 0;
  int can_write_ready = 0;
  int i;

  memset(&p, 0, sizeof(p));
  p.task = task;
  p.in = -1;
  task_update(task, TASK_RUNNING, dst, 0, NULL);

  if(task_cancel_requested(task)) {
    errno = ECANCELED;
    return -1;
  }
  if((p.in = open(src, O_RDONLY)) < 0) {
    goto done;
  }
  if((out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0777)) < 0) {
    goto done;
  }
  if(pthread_mutex_init(&p.lock, NULL)) {
    errno = EAGAIN;
    goto done;
  }
  lock_ready = 1;
  if(pthread_cond_init(&p.can_read, NULL)) {
    errno = EAGAIN;
    goto done;
  }
  can_read_ready = 1;
  if(pthread_cond_init(&p.can_write, NULL)) {
    errno = EAGAIN;
    goto done;
  }
  can_write_ready = 1;
  for(i = 0; i < COPY_PIPELINE_SLOTS; i++) {
    if(posix_memalign((void **)&p.slots[i].data, 4096, COPY_BUFFER_SIZE)) {
      errno = ENOMEM;
      goto done;
    }
  }
  if(pthread_create(&reader, NULL, copy_pipeline_reader, &p)) {
    errno = EAGAIN;
    goto done;
  }
  reader_started = 1;

  for(;;) {
    int slot;
    char *buf;
    size_t size;
    size_t off = 0;

    pthread_mutex_lock(&p.lock);
    while(!p.error && !p.done && !p.slots[p.write_index].ready) {
      pthread_cond_wait(&p.can_write, &p.lock);
    }
    if(p.error) {
      errno = p.error_number;
      pthread_mutex_unlock(&p.lock);
      goto done;
    }
    if(p.done && !p.slots[p.write_index].ready) {
      pthread_mutex_unlock(&p.lock);
      break;
    }
    slot = p.write_index;
    buf = p.slots[slot].data;
    size = p.slots[slot].size;
    pthread_mutex_unlock(&p.lock);

    while(off < size) {
      ssize_t n;
      if(task_cancel_requested(task)) {
        errno = ECANCELED;
        goto done;
      }
      n = write(out, buf + off, size - off);
      if(n <= 0) {
        if(!n) errno = EIO;
        goto done;
      }
      off += (size_t)n;
      task_update(task, TASK_RUNNING, dst, (unsigned long long)n, NULL);
    }

    pthread_mutex_lock(&p.lock);
    p.slots[slot].ready = 0;
    p.write_index = (p.write_index + 1) % COPY_PIPELINE_SLOTS;
    pthread_cond_signal(&p.can_read);
    pthread_mutex_unlock(&p.lock);
  }

  if(fchmod_0777(out)) {
    goto done;
  }
  ret = 0;

done:
  if(reader_started) {
    pthread_mutex_lock(&p.lock);
    p.done = 1;
    p.error = 1;
    pthread_cond_broadcast(&p.can_read);
    pthread_cond_broadcast(&p.can_write);
    pthread_mutex_unlock(&p.lock);
    pthread_join(reader, NULL);
  }
  for(i = 0; i < COPY_PIPELINE_SLOTS; i++) {
    free(p.slots[i].data);
  }
  if(can_write_ready) pthread_cond_destroy(&p.can_write);
  if(can_read_ready) pthread_cond_destroy(&p.can_read);
  if(lock_ready) pthread_mutex_destroy(&p.lock);
  if(p.in >= 0) close(p.in);
  if(out >= 0) {
    if(close(out)) ret = -1;
  }
  if(ret) unlink(dst);
  return ret;
}
#endif

static int
copy_file(file_task_t *task, const char *src, const char *dst) {
#if FILEMGR_PIPELINE_COPY
  struct stat st;

  if(lstat(src, &st)) {
    return -1;
  }
  if(st.st_size >= LARGE_FILE_THRESHOLD) {
    return copy_file_pipeline(task, src, dst);
  }
#endif
  return copy_file_buffered(task, src, dst);
}

static int copy_path(file_task_t *task, const char *src, const char *dst);
static int remove_path(file_task_t *task, const char *path);
static int check_remove_path_writable(file_task_t *task, const char *path);
static int mode_access(const char *path, int mode);

static int
ensure_copy_dir(const char *path) {
  struct stat st;

  if(mkdir(path, 0777)) {
    if(errno != EEXIST) {
      return -1;
    }
    if(lstat(path, &st)) {
      return -1;
    }
    if(!S_ISDIR(st.st_mode)) {
      errno = ENOTDIR;
      return -1;
    }
  }
  return chmod_path_0777(path);
}

static int
check_remove_entry_writable(const char *path) {
  char parent[PATH_MAX];

  if(path_dirname(path, parent, sizeof(parent))) {
    return -1;
  }
  return mode_access(parent, W_OK | X_OK);
}

static int
check_remove_dir_writable(file_task_t *task, const char *path) {
  DIR *dir;
  struct dirent *entry;
  int ret = -1;

  if(mode_access(path, R_OK | W_OK | X_OK)) {
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
finish_copied_move(file_task_t *task, const char *src) {
  int ret;

  task_finish_bytes(task, "checking source permissions");
  ret = check_remove_path_writable(task, src);
  if(!ret) {
    task_finish_bytes(task, "removing source");
    ret = remove_path(task, src);
  }
  return ret;
}

#if FILEMGR_AGGRESSIVE_COPY
static int copy_dir_queued(file_task_t *task, const char *src, const char *dst,
                           copy_queue_t *queue);

static void *
copy_queue_worker(void *arg) {
  copy_queue_t *queue = arg;

  for(;;) {
    copy_job_t *job;
    int ret;

    pthread_mutex_lock(&queue->lock);
    while(!queue->stopping && !queue->head) {
      pthread_cond_wait(&queue->has_work, &queue->lock);
    }
    if(queue->stopping && !queue->head) {
      pthread_mutex_unlock(&queue->lock);
      return NULL;
    }
    job = queue->head;
    queue->head = job->next;
    if(!queue->head) {
      queue->tail = NULL;
    }
    queue->queued--;
    queue->active++;
    pthread_cond_signal(&queue->has_space);
    pthread_mutex_unlock(&queue->lock);

    ret = task_cancel_requested(queue->task) ? -1 :
      copy_file_buffered(queue->task, job->src, job->dst);
    if(ret && task_cancel_requested(queue->task)) {
      errno = ECANCELED;
    }

    pthread_mutex_lock(&queue->lock);
    if(ret) {
      queue->error = 1;
      queue->error_number = errno ? errno : EIO;
      queue->stopping = 1;
      pthread_cond_broadcast(&queue->has_work);
      pthread_cond_broadcast(&queue->has_space);
    }
    queue->active--;
    if(!queue->head && !queue->active) {
      pthread_cond_broadcast(&queue->idle);
    }
    pthread_mutex_unlock(&queue->lock);
    free(job);
  }
}

static int
copy_queue_init(copy_queue_t *queue, file_task_t *task) {
  int i;

  memset(queue, 0, sizeof(*queue));
  queue->task = task;
  if(pthread_mutex_init(&queue->lock, NULL)) {
    return -1;
  }
  if(pthread_cond_init(&queue->has_work, NULL)) {
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  if(pthread_cond_init(&queue->has_space, NULL)) {
    pthread_cond_destroy(&queue->has_work);
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  if(pthread_cond_init(&queue->idle, NULL)) {
    pthread_cond_destroy(&queue->has_space);
    pthread_cond_destroy(&queue->has_work);
    pthread_mutex_destroy(&queue->lock);
    return -1;
  }
  for(i = 0; i < SMALL_COPY_WORKERS; i++) {
    if(pthread_create(&queue->workers[i], NULL, copy_queue_worker, queue)) {
      queue->stopping = 1;
      pthread_cond_broadcast(&queue->has_work);
      while(queue->worker_count > 0) {
        pthread_join(queue->workers[--queue->worker_count], NULL);
      }
      pthread_cond_destroy(&queue->idle);
      pthread_cond_destroy(&queue->has_space);
      pthread_cond_destroy(&queue->has_work);
      pthread_mutex_destroy(&queue->lock);
      return -1;
    }
    queue->worker_count++;
  }
  return 0;
}

static int
copy_queue_enqueue(copy_queue_t *queue, const char *src, const char *dst) {
  copy_job_t *job;

  if(!(job = calloc(1, sizeof(*job)))) {
    return -1;
  }
  snprintf(job->src, sizeof(job->src), "%s", src);
  snprintf(job->dst, sizeof(job->dst), "%s", dst);

  pthread_mutex_lock(&queue->lock);
  while(!queue->stopping && queue->queued >= FILE_TASK_QUEUE_LIMIT) {
    pthread_cond_wait(&queue->has_space, &queue->lock);
  }
  if(queue->stopping || queue->error || task_cancel_requested(queue->task)) {
    pthread_mutex_unlock(&queue->lock);
    free(job);
    errno = queue->error_number ? queue->error_number : ECANCELED;
    return -1;
  }
  if(queue->tail) {
    queue->tail->next = job;
  } else {
    queue->head = job;
  }
  queue->tail = job;
  queue->queued++;
  pthread_cond_signal(&queue->has_work);
  pthread_mutex_unlock(&queue->lock);
  return 0;
}

static int
copy_queue_wait(copy_queue_t *queue) {
  int ret = 0;

  pthread_mutex_lock(&queue->lock);
  while(!queue->error && (queue->head || queue->active)) {
    pthread_cond_wait(&queue->idle, &queue->lock);
  }
  if(queue->error) {
    errno = queue->error_number ? queue->error_number : EIO;
    ret = -1;
  }
  pthread_mutex_unlock(&queue->lock);
  return ret;
}

static int
copy_queue_finish(copy_queue_t *queue, int abort_pending) {
  copy_job_t *job;
  int ret = abort_pending ? -1 : copy_queue_wait(queue);
  int i;

  pthread_mutex_lock(&queue->lock);
  queue->stopping = 1;
  if(abort_pending) {
    while(queue->head) {
      job = queue->head;
      queue->head = job->next;
      free(job);
    }
    queue->tail = NULL;
    queue->queued = 0;
  }
  pthread_cond_broadcast(&queue->has_work);
  pthread_cond_broadcast(&queue->has_space);
  pthread_mutex_unlock(&queue->lock);
  for(i = 0; i < queue->worker_count; i++) {
    pthread_join(queue->workers[i], NULL);
  }
  while(queue->head) {
    job = queue->head;
    queue->head = job->next;
    free(job);
  }
  pthread_cond_destroy(&queue->idle);
  pthread_cond_destroy(&queue->has_space);
  pthread_cond_destroy(&queue->has_work);
  pthread_mutex_destroy(&queue->lock);
  return ret;
}

static int
copy_dir(file_task_t *task, const char *src, const char *dst) {
  copy_queue_t queue;
  int ret;

  if(copy_queue_init(&queue, task)) {
    return -1;
  }
  ret = copy_dir_queued(task, src, dst, &queue);
  if(copy_queue_finish(&queue, ret)) {
    ret = -1;
  }
  return ret;
}

static int
copy_dir_queued(file_task_t *task, const char *src, const char *dst,
                copy_queue_t *queue) {
  DIR *dir;
  struct dirent *entry;
  struct stat st;
  int ret = -1;

  task_update(task, TASK_RUNNING, dst, 0, NULL);

  if(task_cancel_requested(task)) {
    return -1;
  }
  if(ensure_copy_dir(dst)) {
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
       path_join(to, sizeof(to), dst, entry->d_name)) {
      goto done;
    }
    if(lstat(from, &st)) {
      goto done;
    }
    if(S_ISDIR(st.st_mode)) {
      if(copy_dir_queued(task, from, to, queue)) {
        goto done;
      }
    } else if(S_ISREG(st.st_mode)) {
#if FILEMGR_PIPELINE_COPY
      if(st.st_size >= LARGE_FILE_THRESHOLD) {
        if(copy_queue_wait(queue) || copy_file_pipeline(task, from, to)) {
          goto done;
        }
      } else
#endif
      if(copy_queue_enqueue(queue, from, to)) {
        goto done;
      }
    } else {
      errno = ENOTSUP;
      goto done;
    }
  }

  ret = 0;
done:
  closedir(dir);
  return ret;
}
#else
static int
copy_dir(file_task_t *task, const char *src, const char *dst) {
  DIR *dir;
  struct dirent *entry;
  int ret = -1;

  task_update(task, TASK_RUNNING, dst, 0, NULL);

  if(task_cancel_requested(task)) {
    return -1;
  }
  if(ensure_copy_dir(dst)) {
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
#endif

static int
copy_path(file_task_t *task, const char *src, const char *dst) {
  struct stat st;

  if(lstat(src, &st)) {
    return -1;
  }
  if(S_ISDIR(st.st_mode)) {
    return copy_dir(task, src, dst);
  }
  if(S_ISREG(st.st_mode)) {
    return copy_file(task, src, dst);
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
      ret = finish_copied_move(task, src);
    }
    return ret;
  }

  ret = rename(src, dst);
  if(ret && errno == EXDEV) {
    ret = copy_path(task, src, dst);
    if(!ret) {
      ret = finish_copied_move(task, src);
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

static void
set_permission_error_detail(char *error, size_t error_size,
                            char *code, size_t code_size,
                            char *arg, size_t arg_size,
                            const char *error_code, const char *message,
                            const char *path) {
  struct stat st;

  set_error_detail(error, error_size, code, code_size, arg, arg_size,
                   error_code, message, path);
  if(!stat(path, &st)) {
    snprintf(arg, arg_size, "%s (mode=%04o, uid=%lu, gid=%lu)", path,
             (unsigned int)(st.st_mode & 07777),
             (unsigned long)st.st_uid, (unsigned long)st.st_gid);
  }
}

static int
mode_access_stat(const struct stat *st, int mode) {
  mode_t allowed;
  uid_t uid = geteuid();

  if(uid == 0) {
    if(!(mode & X_OK) || !S_ISREG(st->st_mode) ||
       (st->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
      return 0;
    }
  } else if(uid == st->st_uid) {
    allowed = (st->st_mode >> 6) & 7;
    if((allowed & mode) == (mode_t)mode) {
      return 0;
    }
  } else {
    gid_t gid = getegid();
    int group_match = gid == st->st_gid;

    if(!group_match) {
      int count = getgroups(0, NULL);
      gid_t *groups = count > 0 ? malloc((size_t)count * sizeof(*groups)) : NULL;

      if(groups && getgroups(count, groups) == count) {
        int i;
        for(i = 0; i < count; i++) {
          if(groups[i] == st->st_gid) {
            group_match = 1;
            break;
          }
        }
      }
      free(groups);
    }
    allowed = group_match ? (st->st_mode >> 3) & 7 : st->st_mode & 7;
    if((allowed & mode) == (mode_t)mode) {
      return 0;
    }
  }
  errno = EACCES;
  return -1;
}

static int
mode_access(const char *path, int mode) {
  struct stat st;

  return stat(path, &st) ? -1 : mode_access_stat(&st, mode);
}

static int
probe_dir_writable(const char *path) {
  char name[80];
  char probe[PATH_MAX];
  int attempt;

  for(attempt = 0; attempt < 16; attempt++) {
    int fd;
    int error = 0;

    snprintf(name, sizeof(name), ".web-file-mgr-%ld-%lld-%d.tmp",
             (long)getpid(), (long long)time(NULL), attempt);
    if(path_join(probe, sizeof(probe), path, name)) {
      return -1;
    }
    fd = open(probe, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if(fd < 0) {
      if(errno == EEXIST) {
        continue;
      }
      return -1;
    }
    if(close(fd)) {
      error = errno;
    }
    if(unlink(probe) && !error) {
      error = errno;
    }
    if(error) {
      errno = error;
      return -1;
    }
    return 0;
  }
  errno = EEXIST;
  return -1;
}

static int
probe_file_writable(const char *path) {
  int fd = open(path, O_WRONLY);

  return fd < 0 ? -1 : close(fd);
}

static int
check_target_writable(const char *target, char *error, size_t error_size,
                      char *code, size_t code_size, char *arg, size_t arg_size) {
  struct stat st;
  char parent[PATH_MAX];

  if(!stat(target, &st)) {
    if(S_ISDIR(st.st_mode)) {
      if(probe_dir_writable(target)) {
        set_permission_error_detail(error, error_size, code, code_size,
                                    arg, arg_size, "target_dir_not_writable",
                                    "target directory is not writable", target);
        return -1;
      }
    } else {
      if(probe_file_writable(target)) {
        set_permission_error_detail(error, error_size, code, code_size,
                                    arg, arg_size, "target_file_not_writable",
                                    "target file is not writable", target);
        return -1;
      }
      goto check_parent;
    }
    return 0;
  }

  if(errno != ENOENT) {
    set_error_detail(error, error_size, code, code_size, arg, arg_size,
                     "target_check_failed", "cannot check target path", target);
    return -1;
  }

check_parent:
  if(path_dirname(target, parent, sizeof(parent))) {
    set_error_detail(error, error_size, code, code_size, arg, arg_size,
                     "target_check_failed", "cannot check target path", target);
    return -1;
  }
  if(probe_dir_writable(parent)) {
    set_permission_error_detail(error, error_size, code, code_size,
                                arg, arg_size, "target_parent_not_writable",
                                "current directory is not writable", parent);
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

#ifdef __SCE__
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
#endif

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

static void *
task_worker(void *arg) {
  file_task_t *task = arg;
  unsigned long long total = 0;
  unsigned long long required = 0;
  unsigned long long reclaimable = 0;
  int ret = -1;

  task_update(task, TASK_RUNNING, "preparing", 0, NULL);

  if(task_cancel_requested(task)) {
    task_update(task, TASK_CANCELED, task->src, 0, "canceled");
    return NULL;
  }
  if(task->op == TASK_COPY || task->op == TASK_MOVE) {
    char error[160] = {0};
    char code[64] = {0};
    char arg[PATH_MAX + 96] = {0};

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
      unsigned long long before = total;
      int needs_space = task->op == TASK_COPY;
      char target[PATH_MAX];
      const char *compare_target = NULL;

      if(task->op == TASK_COPY || task->op == TASK_MOVE) {
        if(task_target_path(task, task->srcs[i], target, sizeof(target))) {
          task_update(task, TASK_FAILED, task->srcs[i], 0, strerror(errno));
          return NULL;
        }
      }
      if(task->op == TASK_MOVE) {
        needs_space = move_requires_space_check(task->srcs[i], target);
        if(needs_space < 0) {
          task_update(task, TASK_FAILED, task->srcs[i], 0, strerror(errno));
          return NULL;
        }
        if(!needs_space) {
          continue;
        }
      }
      if(needs_space) {
        struct stat target_st;
        if(!lstat(target, &target_st)) {
          compare_target = target;
        } else if(errno != ENOENT) {
          task_update(task, TASK_FAILED, target, 0, strerror(errno));
          return NULL;
        }
      }
      if(count_path_bytes(task, task->srcs[i], task->srcs[i], compare_target,
                          &total, &reclaimable,
                          &task->file_count, &task->dir_count)) {
        if(errno == ECANCELED || task_cancel_requested(task)) {
          task_update(task, TASK_CANCELED, task->srcs[i], 0, "canceled");
        } else {
          if(errno == ENAMETOOLONG) {
            task_set_error_code(task, "path_too_long", NULL);
          }
          task_update(task, TASK_FAILED, task->srcs[i], 0, strerror(errno));
        }
        return NULL;
      }
      if(needs_space) {
        required += total - before;
      }
    }
    task_set_total(task, total);
    required = reclaimable >= required ? 0 : required - reclaimable;

    if(task->op == TASK_COPY || task->op == TASK_MOVE) {
      char error[128] = {0};
      char code[64] = {0};
      char arg[PATH_MAX] = {0};

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
    time_t completed_at = time(NULL);
    pthread_mutex_lock(&g_tasks_lock);
    task->state = TASK_DONE;
    if(task->total) {
      task->done = task->total;
    }
    task->updated_at = completed_at;
    if((task->op == TASK_COPY || task->op == TASK_MOVE) &&
       completed_at - task->created_at >= TRANSFER_ALERT_THRESHOLD) {
      g_last_completion.id = task->id;
      g_last_completion.op = task->op;
      snprintf(g_last_completion.src, sizeof(g_last_completion.src), "%s", task->src);
      g_last_completion.src_count = task->src_count;
      g_last_completion.total = task->total;
      g_last_completion.file_count = task->dir_count ? task->file_count : 0;
      g_last_completion.elapsed = completed_at - task->created_at;
    }
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
  char *path = fs_path_value(query_value(conn, "path"));
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
  time_t now = time(NULL);
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
    strbuf_printf(&b, ",\"src_count\":%zu,\"total\":%llu,\"done\":%llu,\"speed\":%llu,\"eta\":%llu,\"cancel_requested\":%s,\"created_at\":%lld,\"elapsed\":%lld,\"total_elapsed\":%lld,\"updated_at\":%lld}",
                  task->src_count, task->total, task->done, task->speed, task->eta,
                  task->cancel_requested ? "true" : "false",
                  (long long)task->created_at,
                  task->transfer_started_at ? (long long)(now - task->transfer_started_at) : 0LL,
                  task->created_at ? (long long)(now - task->created_at) : 0LL,
                  (long long)task->updated_at);
  }
  strbuf_printf(&b, "],\"now\":%lld,\"completion\":", (long long)now);
  if(g_last_completion.id) {
    strbuf_printf(&b, "{\"id\":%lu,\"op\":\"%s\",\"src\":",
                  g_last_completion.id, task_op_name(g_last_completion.op));
    json_escape(&b, g_last_completion.src);
    strbuf_printf(&b, ",\"src_count\":%zu,\"elapsed\":%lld,\"total\":%llu,\"file_count\":%zu}",
                  g_last_completion.src_count,
                  (long long)g_last_completion.elapsed,
                  g_last_completion.total, g_last_completion.file_count);
  } else {
    strbuf_append(&b, "null");
  }
  remove_finished_tasks_locked();
  pthread_mutex_unlock(&g_tasks_lock);
  strbuf_append(&b, "}");
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

static enum MHD_Result
api_space(struct MHD_Connection *conn) {
#ifdef __SCE__
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
    {"storageM2", "/mnt/ext1"},
    {"storageExtended", "/mnt/ext0"},
  };
#endif
  char *current = fs_path_value(query_value(conn, "path"));
  strbuf_t b = {0};
  int first = 1;

  strbuf_append(&b, "{\"ok\":true,\"spaces\":[");
#ifdef __SCE__
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
#else
  const char *paths[] = {"/", current && strcmp(current, "/") ? current : NULL};
  const char *labels[] = {"storageRoot", "storageCurrent"};
  for(size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    struct statvfs vfs;
    unsigned long block_size;
    unsigned long long free_bytes;
    unsigned long long total_bytes;

    if(!paths[i] || statvfs(paths[i], &vfs)) {
      continue;
    }
    block_size = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    free_bytes = vfs_bytes(vfs.f_bavail, block_size);
    total_bytes = vfs_bytes(vfs.f_blocks, block_size);
    if(!first) {
      strbuf_append(&b, ",");
    }
    first = 0;
    strbuf_append(&b, "{\"label_key\":");
    json_escape(&b, labels[i]);
    strbuf_append(&b, ",\"path\":");
    json_escape(&b, paths[i]);
    strbuf_printf(&b, ",\"free\":%llu,\"total\":%llu,\"current\":%s}",
                  free_bytes, total_bytes, i ? "true" : "false");
  }
#endif
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
  char *dst = fs_path_value(query_value(conn, "dst"));
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
  char *dst = fs_path_value(query_value(conn, "dst"));
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
  char *path = fs_path_value(query_value(conn, "path"));
  char *name = fs_path_value(query_value(conn, "name"));
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
  char *path = fs_path_value(query_value(conn, "path"));
  char *name = fs_path_value(query_value(conn, "name"));
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

static enum MHD_Result
api_text(struct MHD_Connection *conn) {
  char *path = fs_path_value(query_value(conn, "path"));
  char *data;
  size_t size;
  struct stat st;
  unsigned long long version;

  if(has_active_task()) {
    free(path);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  if(!path || !text_extension_allowed(path)) {
    free(path);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST,
                           "file type is not editable");
  }
  if(read_text_file(path, &data, &size, &st)) {
    int error = errno;
    free(path);
    if(error == EFBIG) {
      return send_json_error(conn, MHD_HTTP_CONTENT_TOO_LARGE,
                             "text file is too large");
    }
    errno = error;
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "file not found");
  }
  free(path);
  if(!valid_utf8((const unsigned char *)data, size)) {
    free(data);
    return send_json_error(conn, MHD_HTTP_UNSUPPORTED_MEDIA_TYPE,
                           "file is not valid UTF-8");
  }
  version = text_version((const unsigned char *)data, size);
  return send_text_file(conn, data, size, version);
}

static enum MHD_Result
api_text_create(struct MHD_Connection *conn) {
  char *path = fs_path_value(query_value(conn, "path"));
  char *name = fs_path_value(query_value(conn, "name"));
  char target[PATH_MAX];
  int fd = -1;
  int ret = -1;
  int error = 0;
  int created = 0;

  if(has_active_task()) {
    free(path); free(name);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  if(!path || !name || path_join(target, sizeof(target), path, name)) {
    free(path); free(name);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  if(mode_access(path, W_OK | X_OK)) {
    free(path); free(name);
    return send_json_error(conn, MHD_HTTP_FORBIDDEN,
                           "text file is not writable");
  }
  if((fd = open(target, O_WRONLY | O_CREAT | O_EXCL, 0777)) >= 0) {
    created = 1;
    ret = fchmod_0777(fd);
    if(close(fd) && !ret) ret = -1;
    fd = -1;
  }
  if(ret) {
    error = errno;
    if(fd >= 0) close(fd);
    if(created) unlink(target);
  }
  free(path); free(name);
  if(!ret) {
    return send_json_version(conn,
                             text_version((const unsigned char *)"", 0));
  }
  errno = error;
  return error == EEXIST ?
    send_json_error(conn, MHD_HTTP_CONFLICT, "file already exists") :
    send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, NULL);
}

static int
write_text_atomic(const char *path, const char *body, size_t body_size,
                  mode_t mode) {
  struct timespec now;
  char temp[PATH_MAX];
  size_t written = 0;
  int fd = -1;
  int ret = -1;
  int n;

  clock_gettime(CLOCK_MONOTONIC, &now);
  n = snprintf(temp, sizeof(temp), "%s.wfm-%ld-%ld.tmp", path,
               (long)getpid(), now.tv_nsec);
  if(n < 0 || (size_t)n >= sizeof(temp)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if((fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) {
    return -1;
  }
  while(written < body_size) {
    ssize_t count = write(fd, body + written, body_size - written);
    if(count <= 0) {
      goto done;
    }
    written += (size_t)count;
  }
  if(fchmod(fd, mode & 07777) && !ignore_chmod_error(errno)) {
    goto done;
  }
  if(fsync(fd)) {
    goto done;
  }
  if(close(fd)) {
    fd = -1;
    goto done;
  }
  fd = -1;
  if(rename(temp, path)) {
    goto done;
  }
  ret = 0;

done:
  if(fd >= 0) close(fd);
  if(ret) unlink(temp);
  return ret;
}

static enum MHD_Result
api_text_save(struct MHD_Connection *conn, const char *body,
              size_t body_size) {
  char *path = fs_path_value(query_value(conn, "path"));
  char *expected = query_value(conn, "version");
  char *current = NULL;
  size_t current_size = 0;
  struct stat st;
  char version_text[24];
  char parent[PATH_MAX];
  char *formatted = NULL;
  size_t formatted_size = 0;
  int ret;

  if(has_active_task()) {
    free(path); free(expected);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  if(!path || !expected) {
    free(path); free(expected);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  if(body_size > TEXT_FILE_MAX_SIZE) {
    free(path); free(expected);
    return send_json_error(conn, MHD_HTTP_CONTENT_TOO_LARGE,
                           "text file is too large");
  }
  if(!valid_utf8((const unsigned char *)(body ? body : ""), body_size)) {
    free(path); free(expected);
    return send_json_error(conn, MHD_HTTP_UNSUPPORTED_MEDIA_TYPE,
                           "file is not valid UTF-8");
  }
  if(read_text_file(path, &current, &current_size, &st)) {
    free(path); free(expected);
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "file not found");
  }
  snprintf(version_text, sizeof(version_text), "%016llx",
           text_version((const unsigned char *)current, current_size));
  if(strcmp(expected, version_text)) {
    free(current);
    free(path); free(expected);
    return send_json_error(conn, MHD_HTTP_CONFLICT,
                           "file changed since it was opened");
  }
  if(path_dirname(path, parent, sizeof(parent)) ||
     mode_access(path, W_OK) || mode_access(parent, W_OK | X_OK)) {
    free(current);
    free(path); free(expected);
    return send_json_error(conn, MHD_HTTP_FORBIDDEN,
                           "text file is not writable");
  }
  if(format_text_for_save(body, body_size, (const unsigned char *)current,
                          current_size, &formatted, &formatted_size)) {
    int error = errno;
    free(current);
    free(path); free(expected);
    errno = error;
    return error == EFBIG ?
      send_json_error(conn, MHD_HTTP_CONTENT_TOO_LARGE,
                      "text file is too large") :
      send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, NULL);
  }
  free(current);
  ret = write_text_atomic(path, formatted, formatted_size, st.st_mode);
  free(formatted);
  free(path); free(expected);
  return ret ? send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, NULL)
             : send_json_ok(conn);
}

enum MHD_Result
filemgr_api_request(struct MHD_Connection *conn, const char *url,
                    const char *method, const char *body, size_t body_size) {
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
  if(!strcmp(url, "/api/text")) {
    return strcmp(method, MHD_HTTP_METHOD_GET) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_text(conn);
  }
  if(!strcmp(url, "/api/text/create")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_text_create(conn);
  }
  if(!strcmp(url, "/api/text/save")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_text_save(conn, body, body_size);
  }
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
