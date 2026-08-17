#include "filemgr.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "filemgr_internal.h"
#include "json_util.h"
#include "path_util.h"

#define UPLOAD_BUFFER_SIZE (1024 * 1024)

typedef struct upload_context {
  file_task_t *task;
  int fd;
  char *buffer;
  size_t buffered;
  char temp[PATH_MAX];
  char target[PATH_MAX];
  unsigned long long expected;
  unsigned long long written;
  int failed;
  int error;
  int task_done;
} upload_context_t;

static int
upload_flush(upload_context_t *ctx) {
  size_t written = 0;

  while(written < ctx->buffered) {
    ssize_t n;

    if(ctx->task && task_cancel_requested(ctx->task)) {
      ctx->failed = 1;
      ctx->error = ECANCELED;
      return -1;
    }
    n = write(ctx->fd, ctx->buffer + written, ctx->buffered - written);
    if(n <= 0) {
      ctx->failed = 1;
      ctx->error = errno ? errno : EIO;
      return -1;
    }
    written += (size_t)n;
    ctx->written += (unsigned long long)n;
  }
  if(ctx->task && written) {
    task_update(ctx->task, TASK_RUNNING, ctx->target,
                (unsigned long long)written, NULL);
  }
  ctx->buffered = 0;
  return 0;
}

static void
finish_upload_task(file_task_t *task, task_state_t state, const char *current,
                   const char *error) {
  if(!task) {
    return;
  }
  pthread_mutex_lock(&g_tasks_lock);
  if(task_is_active(task)) {
    time_t completed_at = time(NULL);
    task->state = state;
    if(current) {
      snprintf(task->current, sizeof(task->current), "%s", current);
    }
    if(state == TASK_DONE && task->total) {
      task->done = task->total;
    }
    if(state == TASK_DONE) {
      record_task_completion_locked(task, completed_at);
    }
    if(error) {
      snprintf(task->error, sizeof(task->error), "%s", error);
    }
    task->updated_at = completed_at;
  }
  pthread_mutex_unlock(&g_tasks_lock);
}

static file_task_t *
upload_task_from_conn(struct MHD_Connection *conn) {
  char *idstr = request_value(conn, "X-WFM-Task-ID", "task_id");
  unsigned long id = idstr ? strtoul(idstr, NULL, 10) : 0;
  file_task_t *task = NULL;

  free(idstr);
  if(!id) {
    return NULL;
  }
  pthread_mutex_lock(&g_tasks_lock);
  task = find_task_locked(id);
  if(!task || task->op != TASK_UPLOAD || !task_is_active(task)) {
    task = NULL;
  }
  pthread_mutex_unlock(&g_tasks_lock);
  return task;
}

static int
check_upload_manifest_space(const char *base, const char *rels,
                            const char *sizes, unsigned long long fallback_total,
                            char *error, size_t error_size,
                            char *code, size_t code_size,
                            char *arg, size_t arg_size) {
  char *rels_copy = NULL;
  char *sizes_copy = NULL;
  char *rel;
  char *size_text;
  char *rel_save;
  char *size_save;
  unsigned long long available;
  int ret = -1;

  if(!rels || !sizes) {
    return check_target_space(base, fallback_total, error, error_size,
                              code, code_size, arg, arg_size);
  }
  if(target_available_space(base, &available)) {
    snprintf(error, error_size, "cannot read target free space");
    snprintf(code, code_size, "space_check_failed");
    snprintf(arg, arg_size, "%s", base);
    return -1;
  }
  if(!(rels_copy = strdup(rels)) || !(sizes_copy = strdup(sizes))) {
    errno = ENOMEM;
    goto done;
  }

  rel = strtok_r(rels_copy, "\n", &rel_save);
  size_text = strtok_r(sizes_copy, "\n", &size_save);
  while(rel || size_text) {
    char target[PATH_MAX];
    unsigned long long size;

    if(!rel || !size_text || path_join_relative(target, sizeof(target), base, rel)) {
      snprintf(error, error_size, "invalid path");
      snprintf(code, code_size, "invalid_path");
      snprintf(arg, arg_size, "%s", rel ? rel : "");
      errno = EINVAL;
      goto done;
    }

    size = strtoull(size_text, NULL, 10);
    if(available < size) {
      snprintf(error, error_size,
               "not enough target space, required %llu bytes, available %llu bytes",
               size, available);
      snprintf(code, code_size, "no_space");
      snprintf(arg, arg_size, "%llu,%llu", size, available);
      errno = ENOSPC;
      goto done;
    }

    available -= size;

    rel = strtok_r(NULL, "\n", &rel_save);
    size_text = strtok_r(NULL, "\n", &size_save);
  }

  ret = 0;

done:
  free(rels_copy);
  free(sizes_copy);
  return ret;
}

enum MHD_Result
api_upload_prepare(struct MHD_Connection *conn, const char *body,
                   size_t body_size) {
  char *path = fs_path_value(body_form_value(body, body_size, "path"));
  char *src = body_form_value(body, body_size, "src");
  char *total_text = body_form_value(body, body_size, "total");
  char *count_text = body_form_value(body, body_size, "count");
  char *rels = body_form_value(body, body_size, "rels");
  char *sizes = body_form_value(body, body_size, "sizes");
  char *overwrite = body_form_value(body, body_size, "overwrite");
  file_task_t *task = calloc(1, sizeof(*task));
  strbuf_t b = {0};
  unsigned long long total = total_text ? strtoull(total_text, NULL, 10) : 0;
  size_t count = count_text ? (size_t)strtoull(count_text, NULL, 10) : 0;
  char error[128] = {0};
  char code[64] = {0};
  char arg[PATH_MAX + 96] = {0};

  if(!task) {
    free(path); free(src); free(total_text); free(count_text);
    free(rels); free(sizes); free(overwrite);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
  }
  if(!path || !src || !count) {
    free_task(task);
    free(path); free(src); free(total_text); free(count_text);
    free(rels); free(sizes); free(overwrite);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }

  pthread_mutex_lock(&g_tasks_lock);
  remove_finished_tasks_locked();
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    free_task(task);
    free(path); free(src); free(total_text); free(count_text);
    free(rels); free(sizes); free(overwrite);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  pthread_mutex_unlock(&g_tasks_lock);

  if(check_upload_manifest_space(path, rels, sizes, total, error, sizeof(error),
                                 code, sizeof(code), arg, sizeof(arg))) {
    free_task(task);
    free(path); free(src); free(total_text); free(count_text);
    free(rels); free(sizes); free(overwrite);
    return send_json_error_detail(conn,
                                  errno == ENOSPC ? MHD_HTTP_INSUFFICIENT_STORAGE :
                                  MHD_HTTP_INTERNAL_SERVER_ERROR,
                                  error[0] ? error : NULL,
                                  code[0] ? code : NULL,
                                  arg[0] ? arg : NULL);
  }

  task->op = TASK_UPLOAD;
  task->state = TASK_RUNNING;
  task->src_count = count;
  task->file_count = count;
  task->total = total;
  snprintf(task->src, sizeof(task->src), "%s", src);
  snprintf(task->dst, sizeof(task->dst), "%s", path);
  snprintf(task->current, sizeof(task->current), "%s", src);
  task->created_at = time(NULL);
  task->updated_at = task->created_at;

  pthread_mutex_lock(&g_tasks_lock);
  remove_finished_tasks_locked();
  if(has_active_task_locked()) {
    pthread_mutex_unlock(&g_tasks_lock);
    free_task(task);
    free(path); free(src); free(total_text); free(count_text);
    free(rels); free(sizes); free(overwrite);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  task->id = g_next_task_id++;
  task->next = g_tasks;
  g_tasks = task;
  pthread_mutex_unlock(&g_tasks_lock);

  free(path); free(src); free(total_text); free(count_text);
  free(rels); free(sizes); free(overwrite);
  strbuf_printf(&b, "{\"ok\":true,\"task_id\":%lu}", task->id);
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}

enum MHD_Result
api_upload_finish(struct MHD_Connection *conn) {
  file_task_t *task = upload_task_from_conn(conn);

  if(!task) {
    return send_json_error(conn, MHD_HTTP_NOT_FOUND, "active task not found");
  }
  if(task_cancel_requested(task)) {
    finish_upload_task(task, TASK_CANCELED,
                       task->current[0] ? task->current : task->src,
                       "canceled");
    return send_json_error(conn, MHD_HTTP_CONFLICT, "canceled");
  }
  finish_upload_task(task, TASK_DONE,
                     task->current[0] ? task->current : task->src, NULL);
  return send_json_ok(conn);
}

int
filemgr_upload_begin(struct MHD_Connection *conn, void **upload_ctx) {
  upload_context_t *ctx;
  char *base = fs_path_value(request_value(conn, "X-WFM-Path", "path"));
  char *rel = request_value(conn, "X-WFM-Rel", "rel");
  char *overwrite = request_value(conn, "X-WFM-Overwrite", "overwrite");
  char *size_text = request_value(conn, "X-WFM-Size", "size");
  file_task_t *task = upload_task_from_conn(conn);
  char **checked_dirs = NULL;
  size_t checked_dir_count = 0;
  struct stat st;
  char error[128] = {0};
  char code[64] = {0};
  char arg[PATH_MAX + 96] = {0};
  int n;

  *upload_ctx = NULL;
  if(!(ctx = calloc(1, sizeof(*ctx)))) {
    free(base); free(rel); free(overwrite); free(size_text);
    errno = ENOMEM;
    return -1;
  }
  ctx->fd = -1;
  *upload_ctx = ctx;

  if(size_text) {
    ctx->expected = strtoull(size_text, NULL, 10);
  }
  ctx->task = task;
  if(task && task_cancel_requested(task)) {
    ctx->failed = 1;
    ctx->error = ECANCELED;
    goto fail;
  }
  if(!task && has_active_task()) {
    ctx->failed = 1;
    ctx->error = EBUSY;
    goto fail;
  }
  if(!base || !rel || path_join_relative(ctx->target, sizeof(ctx->target),
                                         base, rel)) {
    ctx->failed = 1;
    ctx->error = errno ? errno : EINVAL;
    goto fail;
  }
  if(ensure_parent_dirs(base, rel)) {
    ctx->failed = 1;
    ctx->error = errno ? errno : EACCES;
    goto fail;
  }
  if(!lstat(ctx->target, &st)) {
    if(S_ISDIR(st.st_mode) || !overwrite || strcmp(overwrite, "1")) {
      ctx->failed = 1;
      ctx->error = S_ISDIR(st.st_mode) ? EISDIR : EEXIST;
      goto fail;
    }
  } else if(errno != ENOENT) {
    ctx->failed = 1;
    ctx->error = errno;
    goto fail;
  }
  if(check_target_writable(ctx->target, &checked_dirs, &checked_dir_count,
                           error, sizeof(error), code, sizeof(code),
                           arg, sizeof(arg))) {
    ctx->failed = 1;
    ctx->error = errno ? errno : EACCES;
    goto fail;
  }
  if(check_target_space(ctx->target, ctx->expected, error, sizeof(error),
                        code, sizeof(code), arg, sizeof(arg))) {
    ctx->failed = 1;
    ctx->error = errno ? errno : ENOSPC;
    goto fail;
  }
  n = snprintf(ctx->temp, sizeof(ctx->temp), "%s.wfm-upload-%ld-%lld.tmp",
               ctx->target, (long)getpid(), (long long)time(NULL));
  if(n < 0 || (size_t)n >= sizeof(ctx->temp)) {
    ctx->failed = 1;
    ctx->error = ENAMETOOLONG;
    goto fail;
  }
  ctx->fd = open(ctx->temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if(ctx->fd < 0) {
    ctx->failed = 1;
    ctx->error = errno;
    goto fail;
  }
  if(!(ctx->buffer = malloc(UPLOAD_BUFFER_SIZE))) {
    ctx->failed = 1;
    ctx->error = ENOMEM;
    goto fail;
  }

fail:
  free_paths(checked_dirs, checked_dir_count);
  free(base); free(rel); free(overwrite); free(size_text);
  if(ctx->failed) {
    if(ctx->task) {
      finish_upload_task(ctx->task,
                         ctx->error == ECANCELED ? TASK_CANCELED : TASK_FAILED,
                         ctx->target[0] ? ctx->target : ctx->task->current,
                         ctx->error == ECANCELED ? "canceled" : strerror(ctx->error ? ctx->error : EIO));
      ctx->task_done = 1;
    }
    if(ctx->fd >= 0) {
      close(ctx->fd);
      ctx->fd = -1;
    }
    if(ctx->temp[0]) {
      unlink(ctx->temp);
    }
    errno = ctx->error ? ctx->error : EIO;
    return -1;
  }
  return 0;
}

int
filemgr_upload_data(void *upload_ctx, const char *data, size_t size) {
  upload_context_t *ctx = upload_ctx;

  if(!ctx || ctx->failed) {
    return -1;
  }
  if(ctx->task && task_cancel_requested(ctx->task)) {
    ctx->failed = 1;
    ctx->error = ECANCELED;
    return -1;
  }
  while(size) {
    size_t space = UPLOAD_BUFFER_SIZE - ctx->buffered;
    size_t take = size < space ? size : space;

    memcpy(ctx->buffer + ctx->buffered, data, take);
    ctx->buffered += take;
    data += take;
    size -= take;
    if(ctx->buffered == UPLOAD_BUFFER_SIZE && upload_flush(ctx)) {
      return -1;
    }
  }
  return 0;
}

enum MHD_Result
filemgr_upload_finish(struct MHD_Connection *conn, void *upload_ctx) {
  upload_context_t *ctx = upload_ctx;
  int ret = -1;

  if(!ctx) {
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid path");
  }
  if(ctx->failed) {
    if(ctx->task && !ctx->task_done) {
      finish_upload_task(ctx->task,
                         ctx->error == ECANCELED ? TASK_CANCELED : TASK_FAILED,
                         ctx->target[0] ? ctx->target : ctx->task->current,
                         ctx->error == ECANCELED ? "canceled" : strerror(ctx->error ? ctx->error : EIO));
      ctx->task_done = 1;
    }
    errno = ctx->error ? ctx->error : EIO;
    return send_json_error(conn, errno == EEXIST ? MHD_HTTP_CONFLICT :
                           errno == EBUSY ? MHD_HTTP_CONFLICT :
                           errno == ECANCELED ? MHD_HTTP_CONFLICT :
                           errno == ENOSPC ? MHD_HTTP_INSUFFICIENT_STORAGE :
                           MHD_HTTP_INTERNAL_SERVER_ERROR, NULL);
  }
  if(ctx->buffered && upload_flush(ctx)) {
    ctx->error = ctx->error ? ctx->error : EIO;
    goto done;
  }
  if(ctx->expected && ctx->written != ctx->expected) {
    ctx->error = EIO;
    goto done;
  }
  if(fchmod_0777(ctx->fd)) {
    ctx->error = errno;
    goto done;
  }
  if(fsync(ctx->fd)) {
    ctx->error = errno;
    goto done;
  }
  if(close(ctx->fd)) {
    ctx->fd = -1;
    ctx->error = errno;
    goto done;
  }
  ctx->fd = -1;
  if(rename(ctx->temp, ctx->target)) {
    ctx->error = errno;
    goto done;
  }
  ret = 0;

done:
  if(ctx->fd >= 0) {
    close(ctx->fd);
    ctx->fd = -1;
  }
  if(ret) {
    if(ctx->temp[0]) {
      unlink(ctx->temp);
    }
    if(ctx->task && !ctx->task_done) {
      finish_upload_task(ctx->task,
                         ctx->error == ECANCELED ? TASK_CANCELED : TASK_FAILED,
                         ctx->target[0] ? ctx->target : ctx->task->current,
                         ctx->error == ECANCELED ? "canceled" : strerror(ctx->error ? ctx->error : EIO));
      ctx->task_done = 1;
    }
    errno = ctx->error ? ctx->error : EIO;
    return send_json_error(conn, errno == ECANCELED ? MHD_HTTP_CONFLICT :
                           MHD_HTTP_INTERNAL_SERVER_ERROR, NULL);
  }
  return send_json_ok(conn);
}

void
filemgr_upload_free(void *upload_ctx) {
  upload_context_t *ctx = upload_ctx;

  if(!ctx) {
    return;
  }
  if(ctx->fd >= 0) {
    close(ctx->fd);
    if(ctx->temp[0]) {
      unlink(ctx->temp);
    }
    if(ctx->task && !ctx->task_done) {
      int canceled = task_cancel_requested(ctx->task);
      finish_upload_task(ctx->task, canceled ? TASK_CANCELED : TASK_FAILED,
                         ctx->target[0] ? ctx->target : ctx->task->current,
                         canceled ? "canceled" : "client disconnected");
      ctx->task_done = 1;
    }
  }
  free(ctx->buffer);
  free(ctx);
}
