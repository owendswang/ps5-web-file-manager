#include "filemgr.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "filemgr_internal.h"
#include "json_util.h"
#include "path_util.h"
#include "pkg_installer.h"
#include "websrv.h"

#define COPY_BUFFER_SIZE (8 * 1024 * 1024)
#define COPY_PIPELINE_SLOTS 3
#define FILEMGR_AGGRESSIVE_COPY 0
#define FILEMGR_PIPELINE_COPY 1
#define SMALL_COPY_WORKERS 3
#define FILE_TASK_QUEUE_LIMIT 128
#define LARGE_FILE_THRESHOLD (256LL * 1024 * 1024)
#define TRANSFER_ALERT_THRESHOLD (10 * 60)

typedef struct task_completion {
  unsigned long id;
  task_op_t op;
  char src[PATH_MAX];
  size_t src_count;
  unsigned long long total;
  size_t file_count;
  time_t elapsed;
} task_completion_t;

static int ensure_copy_dir(const char *path);
static int open_copy_temp(const char *dst, char *temp, size_t temp_size);
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

static task_completion_t g_last_completion;

void
record_task_completion_locked(file_task_t *task, time_t completed_at) {
  if((task->op == TASK_COPY || task->op == TASK_MOVE || task->op == TASK_UPLOAD) &&
     completed_at - task->created_at >= TRANSFER_ALERT_THRESHOLD) {
    g_last_completion.id = task->id;
    g_last_completion.op = task->op;
    snprintf(g_last_completion.src, sizeof(g_last_completion.src), "%s", task->src);
    g_last_completion.src_count = task->src_count;
    g_last_completion.total = task->total;
    g_last_completion.file_count = task->dir_count ? task->file_count :
                                   task->op == TASK_UPLOAD ? task->file_count : 0;
    g_last_completion.elapsed = completed_at - task->created_at;
  }
}

int
ensure_parent_dirs(const char *base, const char *rel) {
  char current[PATH_MAX];
  const char *p = rel;
  size_t base_len;

  if(!relative_path_safe(rel)) {
    return -1;
  }
  if(strlen(base) >= sizeof(current)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  strcpy(current, base);
  base_len = strlen(current);
  while(*p) {
    const char *slash = strchr(p, '/');
    size_t len;

    if(!slash) {
      return 0;
    }
    len = (size_t)(slash - rel);
    if(base_len + (strcmp(base, "/") ? 1 : 0) + len >= sizeof(current)) {
      errno = ENAMETOOLONG;
      return -1;
    }
    snprintf(current, sizeof(current), "%s%s%.*s", base,
             strcmp(base, "/") ? "/" : "", (int)len, rel);
    if(ensure_copy_dir(current)) {
      return -1;
    }
    p = slash + 1;
  }
  return 0;
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

enum MHD_Result
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

enum MHD_Result
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

enum MHD_Result
send_json_error(struct MHD_Connection *conn, unsigned int status,
                const char *msg) {
  return send_json_error_detail(conn, status, msg, api_error_code(msg), NULL);
}

enum MHD_Result
send_json_error_detail(struct MHD_Connection *conn, unsigned int status,
                       const char *msg, const char *code, const char *arg) {
  strbuf_t b = {0};
  const char *fallback = msg ? msg : strerror(errno);

  strbuf_append(&b, "{\"ok\":false,\"error\":");
  json_escape(&b, fallback);
  strbuf_append(&b, ",\"error_code\":");
  json_escape(&b, code ? code : api_error_code(msg));
  strbuf_append(&b, ",\"error_arg\":");
  json_escape(&b, arg ? arg : fallback);
  strbuf_append(&b, "}");
  return send_buffer(conn, status, b.data, "application/json");
}

static int task_target_path(file_task_t *task, const char *src,
                            char *out, size_t size);

static int count_path_bytes_sync(file_task_t *task, const char *path,
                                 const char *display,
                                 unsigned long long *total,
                                 size_t *file_count, size_t *dir_count);

static int
count_dir_bytes_sync(file_task_t *task, const char *path, const char *display,
                     unsigned long long *total, size_t *file_count,
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
    if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
      continue;
    }
    if(task && task_cancel_requested(task)) {
      goto done;
    }
    if(path_join(child, sizeof(child), path, entry->d_name) ||
       (display && path_join(display_child, sizeof(display_child), display,
                             entry->d_name)) ||
       count_path_bytes_sync(task, child, display ? display_child : NULL,
                             total, file_count, dir_count)) {
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
                      unsigned long long *total, size_t *file_count,
                      size_t *dir_count) {
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
    if(dir_count) (*dir_count)++;
    return count_dir_bytes_sync(task, path, display, total, file_count,
                                dir_count);
  }
  if(S_ISREG(st.st_mode)) {
    *total += (unsigned long long)st.st_size;
    if(file_count) (*file_count)++;
  }
  return 0;
}

#if FILEMGR_AGGRESSIVE_COPY
typedef struct count_job {
  char path[PATH_MAX];
  char display[PATH_MAX];
  int has_display;
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
  size_t file_count;
  size_t dir_count;
} count_queue_t;

static void
count_queue_add(count_queue_t *queue, unsigned long long total, size_t file_count,
                size_t dir_count) {
  pthread_mutex_lock(&queue->lock);
  queue->total += total;
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
                                &total, &file_count, &dir_count);
    if(!ret) {
      count_queue_add(queue, total, file_count, dir_count);
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
count_queue_enqueue(count_queue_t *queue, const char *path, const char *display) {
  count_job_t *job;

  if(!(job = calloc(1, sizeof(*job)))) {
    return -1;
  }
  snprintf(job->path, sizeof(job->path), "%s", path);
  if(display) {
    snprintf(job->display, sizeof(job->display), "%s", display);
    job->has_display = 1;
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
                 unsigned long long *total, size_t *file_count,
                 size_t *dir_count) {
  DIR *dir;
  struct dirent *entry;
  struct stat st;
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
    return count_path_bytes_sync(task, path, display, total, file_count,
                                 dir_count);
  }
  if(task) {
    task_update(task, TASK_RUNNING, display ? display : path, 0, NULL);
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
       count_queue_enqueue(&queue, child, display ? display_child : NULL)) {
      goto done;
    }
  }

  ret = count_queue_finish(&queue, 0);
  queue_finished = 1;
  if(!ret) {
    *total += queue.total;
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

int
count_task_path_bytes(file_task_t *task, const char *path, const char *display,
                      unsigned long long *total, size_t *file_count,
                      size_t *dir_count) {
  return count_path_bytes(task, path, display, total, file_count, dir_count);
}

static int
open_copy_temp(const char *dst, char *temp, size_t temp_size) {
  char parent[PATH_MAX];
  char name[96];
  int attempt;

  if(path_dirname(dst, parent, sizeof(parent))) {
    return -1;
  }
  for(attempt = 0; attempt < 32; attempt++) {
    int fd;

    snprintf(name, sizeof(name), ".wfm-copy-%ld-%lld-%d.tmp",
             (long)getpid(), (long long)time(NULL), attempt);
    if(path_join(temp, temp_size, parent, name)) {
      return -1;
    }
    fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if(fd >= 0) {
      return fd;
    }
    if(errno != EEXIST) {
      return -1;
    }
  }
  errno = EEXIST;
  return -1;
}

static int
copy_file_buffered(file_task_t *task, const char *src, const char *dst) {
  char *buf = NULL;
  char temp[PATH_MAX] = {0};
  int in = -1;
  int out = -1;
  int ret = -1;
  ssize_t n;

  task_update(task, TASK_RUNNING, dst, 0, NULL);

  if(task_cancel_requested(task)) {
    errno = ECANCELED;
    return -1;
  }
  if((in = open(src, O_RDONLY)) < 0) {
    goto done;
  }
  if((out = open_copy_temp(dst, temp, sizeof(temp))) < 0) {
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
      errno = ECANCELED;
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
  if(task_cancel_requested(task)) {
    errno = ECANCELED;
    goto done;
  }
  ret = 0;

done:
  free(buf);
  if(in >= 0) close(in);
  if(out >= 0) {
    if(close(out)) ret = -1;
  }
  if(!ret && task_cancel_requested(task)) {
    errno = ECANCELED;
    ret = -1;
  }
  if(!ret && rename(temp, dst)) {
    ret = -1;
  }
  if(ret && temp[0]) {
    unlink(temp);
  }
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
  char temp[PATH_MAX] = {0};
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
  if((out = open_copy_temp(dst, temp, sizeof(temp))) < 0) {
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
  if(task_cancel_requested(task)) {
    errno = ECANCELED;
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
  if(!ret && task_cancel_requested(task)) {
    errno = ECANCELED;
    ret = -1;
  }
  if(!ret && rename(temp, dst)) {
    ret = -1;
  }
  if(ret && temp[0]) {
    unlink(temp);
  }
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
check_task_targets_writable(file_task_t *task, char *error, size_t error_size,
                            char *code, size_t code_size, char *arg, size_t arg_size) {
  char **checked_dirs = NULL;
  size_t checked_dir_count = 0;
  size_t i;

  for(i = 0; i < task->src_count; i++) {
    char target[PATH_MAX];

    if(task_cancel_requested(task)) {
      free_paths(checked_dirs, checked_dir_count);
      return -1;
    }
    if(task_target_path(task, task->srcs[i], target, sizeof(target))) {
      snprintf(error, error_size, "target path is too long");
      snprintf(code, code_size, "target_path_too_long");
      free_paths(checked_dirs, checked_dir_count);
      return -1;
    }
    if(check_target_writable(target, &checked_dirs, &checked_dir_count,
                             error, error_size, code, code_size, arg, arg_size)) {
      free_paths(checked_dirs, checked_dir_count);
      return -1;
    }
  }
  free_paths(checked_dirs, checked_dir_count);
  return 0;
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

static void *
task_worker(void *arg) {
  file_task_t *task = arg;
  unsigned long long total = 0;
  unsigned long long required = 0;
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
  if(task->op == TASK_COPY || task->op == TASK_MOVE) {
    size_t i;
    for(i = 0; i < task->src_count; i++) {
      unsigned long long before = total;
      int needs_space = task->op == TASK_COPY;
      char target[PATH_MAX];

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
      if(count_path_bytes(task, task->srcs[i], task->srcs[i], &total,
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
    record_task_completion_locked(task, completed_at);
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
      if(task->op == TASK_DOWNLOAD && task->state == TASK_QUEUED) {
        task->state = TASK_CANCELED;
        snprintf(task->error, sizeof(task->error), "canceled");
      }
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
stop_websrv_later(void *arg) {
  (void)arg;
  usleep(250000);
  websrv_stop();
  return NULL;
}

static enum MHD_Result
api_exit(struct MHD_Connection *conn) {
  pthread_t thread;

  if(!pthread_create(&thread, NULL, stop_websrv_later, NULL)) {
    pthread_detach(thread);
  }
  return send_json_ok(conn);
}

static enum MHD_Result
api_copy(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *paths_raw = body_form_value(body, body_size, "paths");
  char *dst = fs_path_value(body_form_value(body, body_size, "dst"));
  char *overwrite = body_form_value(body, body_size, "overwrite");
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
api_move(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *paths_raw = body_form_value(body, body_size, "paths");
  char *dst = fs_path_value(body_form_value(body, body_size, "dst"));
  char *overwrite = body_form_value(body, body_size, "overwrite");
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
api_delete(struct MHD_Connection *conn, const char *body, size_t body_size) {
  char *paths_raw = body_form_value(body, body_size, "paths");
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
api_install_pkg(struct MHD_Connection *conn) {
  char *path = fs_path_value(query_value(conn, "path"));
  const char *extension;
  struct stat st;
  char error_code[16];
  int result;

  if(!path || !(extension = strrchr(path, '.')) ||
     strcasecmp(extension, ".pkg")) {
    free(path);
    return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST,
                                  "file is not a PKG package",
                                  "pkg_type_invalid", NULL);
  }
  if(stat(path, &st) || !S_ISREG(st.st_mode)) {
    free(path);
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "file not found");
  }

  result = pkg_installer_install(path);
  free(path);
  if(result == PKG_INSTALL_UNSUPPORTED) {
    return send_json_error_detail(conn, MHD_HTTP_NOT_IMPLEMENTED,
                                  "package installation is only available on PS5",
                                  "pkg_install_unsupported", NULL);
  }
  if(result) {
    snprintf(error_code, sizeof(error_code), "0x%08X", (unsigned int)result);
    return send_json_error_detail(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                  "package installation failed",
                                  "pkg_install_failed", error_code);
  }
  return send_json_ok(conn);
}

enum MHD_Result
filemgr_api_request(struct MHD_Connection *conn, const char *url,
                    const char *method, const char *body, size_t body_size) {
  if(!strcmp(url, "/api/list")) return api_list(conn);
  if(!strcmp(url, "/api/tasks")) return api_tasks(conn);
  if(!strcmp(url, "/api/space")) return api_space(conn);
  if(!strcmp(url, "/api/cancel")) return api_cancel(conn);
  if(!strcmp(url, "/api/exit")) return api_exit(conn);
  if(!strcmp(url, "/api/copy")) return api_copy(conn, body, body_size);
  if(!strcmp(url, "/api/move")) return api_move(conn, body, body_size);
  if(!strcmp(url, "/api/delete")) return api_delete(conn, body, body_size);
  if(!strcmp(url, "/api/download/prepare")) return api_download_prepare(conn, body, body_size);
  if(!strcmp(url, "/api/download")) {
    return strcmp(method, MHD_HTTP_METHOD_GET) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_download(conn);
  }
  if(!strcmp(url, "/api/upload/prepare")) return api_upload_prepare(conn, body, body_size);
  if(!strcmp(url, "/api/upload/finish")) return api_upload_finish(conn);
  if(!strcmp(url, "/api/rename")) return api_rename(conn);
  if(!strcmp(url, "/api/mkdir")) return api_mkdir(conn);
  if(!strcmp(url, "/api/install-pkg")) {
    return strcmp(method, MHD_HTTP_METHOD_POST) ?
      send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method") :
      api_install_pkg(conn);
  }
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
