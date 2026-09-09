#include "upload_fast.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "filemgr_internal.h"
#include "path_util.h"

#define FAST_UPLOAD_BUFFER_SIZE (4 * 1024 * 1024)
#define FAST_UPLOAD_CHUNK_SIZE (32 * 1024 * 1024)

typedef struct fast_upload_context {
  int fd;
  char *buffer;
  size_t buffered;
  off_t offset;
  off_t expected;
  off_t received;
  file_task_t *task;
  char temp[PATH_MAX];
  char target[PATH_MAX];
  int failed;
  unsigned int status;
  char code[64];
  char error[160];
} fast_upload_context_t;

typedef struct fast_upload_paths {
  char target[PATH_MAX];
  char temp[PATH_MAX];
  char map[PATH_MAX];
  off_t size;
  off_t chunk_size;
  off_t chunk_count;
  int overwrite;
} fast_upload_paths_t;

static void
fast_fail(fast_upload_context_t *ctx, unsigned int status,
          const char *code, const char *message) {
  if(ctx->failed) {
    return;
  }
  ctx->failed = 1;
  ctx->status = status;
  snprintf(ctx->code, sizeof(ctx->code), "%s", code ? code : "system_error");
  snprintf(ctx->error, sizeof(ctx->error), "%s",
           message ? message : "upload failed");
}

static int
fast_parse_off(const char *value, off_t *out) {
  unsigned long long number;
  char *end;

  if(!value || !value[0]) {
    return -1;
  }
  errno = 0;
  number = strtoull(value, &end, 10);
  if(errno == ERANGE || end == value || *end ||
     number > (unsigned long long)LLONG_MAX) {
    return -1;
  }
  *out = (off_t)number;
  return 0;
}

static off_t
fast_chunk_count(off_t size, off_t chunk_size) {
  if(size <= 0 || chunk_size <= 0) {
    return 0;
  }
  return (size + chunk_size - 1) / chunk_size;
}

static off_t
fast_chunk_bytes(off_t size, off_t chunk_size, off_t index) {
  off_t offset = index * chunk_size;
  off_t left = size - offset;

  if(left <= 0) {
    return 0;
  }
  return left < chunk_size ? left : chunk_size;
}

static file_task_t *
fast_task_from_query(struct MHD_Connection *conn) {
  char *id_text = query_value(conn, "task_id");
  unsigned long id;
  file_task_t *task;

  if(!id_text) {
    return NULL;
  }
  id = strtoul(id_text, NULL, 10);
  free(id_text);
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
fast_resolve_paths(struct MHD_Connection *conn, fast_upload_paths_t *paths) {
  char *base = fs_path_value(query_value(conn, "path"));
  char *name = query_value(conn, "name");
  char *size_text = query_value(conn, "size");
  char *chunk_text = query_value(conn, "chunk_size");
  char *overwrite_text = query_value(conn, "overwrite");
  struct stat st;
  int ret = -1;
  int n;

  memset(paths, 0, sizeof(*paths));
  paths->chunk_size = FAST_UPLOAD_CHUNK_SIZE;
  paths->overwrite = overwrite_text && !strcmp(overwrite_text, "1");
  if(!size_text || fast_parse_off(size_text, &paths->size) || paths->size < 0 ||
     (chunk_text && fast_parse_off(chunk_text, &paths->chunk_size)) ||
     paths->chunk_size <= 0 || !base || !name ||
     path_join_relative(paths->target, sizeof(paths->target), base, name)) {
    goto done;
  }
  n = snprintf(paths->temp, sizeof(paths->temp), "%s.wfm-upload.part",
               paths->target);
  if(n < 0 || (size_t)n >= sizeof(paths->temp)) {
    goto done;
  }
  n = snprintf(paths->map, sizeof(paths->map), "%s.wfm-upload.map",
               paths->target);
  if(n < 0 || (size_t)n >= sizeof(paths->map)) {
    goto done;
  }
  paths->chunk_count = fast_chunk_count(paths->size, paths->chunk_size);
  if(!lstat(paths->target, &st)) {
    if(S_ISDIR(st.st_mode) || !paths->overwrite) {
      errno = EEXIST;
      goto done;
    }
  } else if(errno != ENOENT) {
    goto done;
  }
  if(!lstat(paths->temp, &st) && !S_ISREG(st.st_mode)) {
    errno = EEXIST;
    goto done;
  }
  ret = 0;

done:
  free(base);
  free(name);
  free(size_text);
  free(chunk_text);
  free(overwrite_text);
  return ret;
}

static int
fast_read_map(const char *path, off_t count, unsigned char **out) {
  unsigned char *done;
  FILE *file;
  unsigned long long index;

  *out = NULL;
  if(count <= 0) {
    return 0;
  }
  done = calloc((size_t)count, 1);
  if(!done) {
    return -1;
  }
  file = fopen(path, "r");
  if(file) {
    while(fscanf(file, "%llu", &index) == 1) {
      if(index < (unsigned long long)count) {
        done[index] = 1;
      }
    }
    fclose(file);
  }
  *out = done;
  return 0;
}

static int
fast_mark_map(const char *path, off_t index) {
  char line[48];
  int len = snprintf(line, sizeof(line), "%lld\n", (long long)index);
  int fd;
  ssize_t written;
  int ret = 0;

  if(len < 0 || (size_t)len >= sizeof(line)) {
    return -1;
  }
  fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if(fd < 0) {
    return -1;
  }
  written = write(fd, line, (size_t)len);
  if(written != (ssize_t)len || fsync(fd)) {
    ret = -1;
  }
  if(close(fd)) {
    ret = -1;
  }
  return ret;
}

static int
fast_all_done(const unsigned char *done, off_t count) {
  off_t i;

  for(i = 0; i < count; i++) {
    if(!done || !done[i]) {
      return 0;
    }
  }
  return 1;
}

static int
fast_flush(fast_upload_context_t *ctx) {
  size_t written = 0;

  while(written < ctx->buffered) {
    ssize_t n = pwrite(ctx->fd, ctx->buffer + written,
                       ctx->buffered - written,
                       ctx->offset + ctx->received + (off_t)written);
    if(n <= 0) {
      fast_fail(ctx, MHD_HTTP_INTERNAL_SERVER_ERROR, "system_error",
                strerror(errno));
      return -1;
    }
    written += (size_t)n;
  }
  if(ctx->task && written) {
    task_update(ctx->task, TASK_RUNNING, ctx->target,
                (unsigned long long)written, NULL);
  }
  ctx->received += (off_t)written;
  ctx->buffered = 0;
  return 0;
}

int
fast_upload_is_chunk_request(const char *url, const char *method) {
  return !strcmp(url, "/api/upload-chunk") &&
         !strcmp(method, MHD_HTTP_METHOD_POST);
}

int
fast_upload_begin(struct MHD_Connection *conn, void **upload_ctx) {
  fast_upload_context_t *ctx;
  fast_upload_paths_t paths;
  char *offset_text;
  off_t offset;
  off_t index;
  char *base;
  char *name;

  *upload_ctx = NULL;
  if(!(ctx = calloc(1, sizeof(*ctx)))) {
    return -1;
  }
  ctx->fd = -1;
  *upload_ctx = ctx;
  if(fast_resolve_paths(conn, &paths)) {
    fast_fail(ctx, MHD_HTTP_BAD_REQUEST, "invalid_path", "invalid upload path");
    return 0;
  }
  offset_text = query_value(conn, "offset");
  if(fast_parse_off(offset_text, &offset) || offset < 0 ||
     (paths.size > 0 && offset >= paths.size) ||
     offset % paths.chunk_size) {
    free(offset_text);
    fast_fail(ctx, MHD_HTTP_BAD_REQUEST, "invalid_path", "invalid upload chunk");
    return 0;
  }
  free(offset_text);
  index = offset / paths.chunk_size;
  ctx->offset = offset;
  ctx->expected = fast_chunk_bytes(paths.size, paths.chunk_size, index);
  if(paths.size <= 0 || ctx->expected <= 0) {
    fast_fail(ctx, MHD_HTTP_BAD_REQUEST, "invalid_path", "invalid upload chunk");
    return 0;
  }
  base = fs_path_value(query_value(conn, "path"));
  name = query_value(conn, "name");
  if(!base || !name || ensure_parent_dirs(base, name)) {
    free(base);
    free(name);
    fast_fail(ctx, MHD_HTTP_FORBIDDEN, "target_parent_not_writable",
              "target parent is not writable");
    return 0;
  }
  free(base);
  free(name);
  ctx->task = fast_task_from_query(conn);
  if(ctx->task && task_cancel_requested(ctx->task)) {
    fast_fail(ctx, MHD_HTTP_CONFLICT, "canceled", "canceled");
    return 0;
  }
  ctx->fd = open(paths.temp, O_WRONLY | O_CREAT, 0600);
  if(ctx->fd < 0) {
    fast_fail(ctx, MHD_HTTP_INTERNAL_SERVER_ERROR, "system_error", strerror(errno));
    return 0;
  }
  memcpy(ctx->target, paths.target, sizeof(ctx->target));
  memcpy(ctx->temp, paths.temp, sizeof(ctx->temp));
  ctx->buffer = malloc(FAST_UPLOAD_BUFFER_SIZE);
  if(!ctx->buffer) {
    fast_fail(ctx, MHD_HTTP_INTERNAL_SERVER_ERROR, "out_of_memory", "out of memory");
  }
  return 0;
}

int
fast_upload_data(void *upload_ctx, const char *data, size_t size) {
  fast_upload_context_t *ctx = upload_ctx;

  if(!ctx || ctx->failed) {
    return -1;
  }
  if(ctx->task && task_cancel_requested(ctx->task)) {
    fast_fail(ctx, MHD_HTTP_CONFLICT, "canceled", "canceled");
    return -1;
  }
  if(ctx->received + (off_t)ctx->buffered + (off_t)size > ctx->expected) {
    fast_fail(ctx, MHD_HTTP_BAD_REQUEST, "upload_failed", "upload chunk is too large");
    return -1;
  }
  while(size) {
    size_t space = FAST_UPLOAD_BUFFER_SIZE - ctx->buffered;
    size_t take = size < space ? size : space;
    memcpy(ctx->buffer + ctx->buffered, data, take);
    ctx->buffered += take;
    data += take;
    size -= take;
    if(ctx->buffered == FAST_UPLOAD_BUFFER_SIZE && fast_flush(ctx)) {
      return -1;
    }
  }
  return 0;
}

enum MHD_Result
fast_upload_finish(struct MHD_Connection *conn, void *upload_ctx) {
  fast_upload_context_t *ctx = upload_ctx;
  enum MHD_Result ret;

  if(!ctx) {
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid upload chunk");
  }
  if(!ctx->failed && ctx->buffered) {
    fast_flush(ctx);
  }
  if(!ctx->failed && ctx->received != ctx->expected) {
    fast_fail(ctx, MHD_HTTP_BAD_REQUEST, "upload_failed", "upload chunk incomplete");
  }
  if(ctx->fd >= 0) {
    if(!ctx->failed && fsync(ctx->fd)) {
      fast_fail(ctx, MHD_HTTP_INTERNAL_SERVER_ERROR, "system_error", strerror(errno));
    }
    close(ctx->fd);
    ctx->fd = -1;
  }
  if(ctx->failed) {
    return send_json_error_detail(conn,
                                  ctx->status ? ctx->status : MHD_HTTP_INTERNAL_SERVER_ERROR,
                                  ctx->error, ctx->code, ctx->target);
  }
  {
    fast_upload_paths_t paths;
    if(fast_resolve_paths(conn, &paths)) {
      return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid upload path");
    }
    if(fast_mark_map(paths.map, ctx->offset / paths.chunk_size)) {
      return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "cannot save upload state");
    }
  }
  ret = send_json_ok(conn);
  return ret;
}

void
fast_upload_free(void *upload_ctx) {
  fast_upload_context_t *ctx = upload_ctx;

  if(!ctx) {
    return;
  }
  if(ctx->fd >= 0) {
    close(ctx->fd);
  }
  free(ctx->buffer);
  free(ctx);
}

enum MHD_Result
fast_upload_status(struct MHD_Connection *conn, const char *method) {
  fast_upload_paths_t paths;
  unsigned char *done = NULL;
  char *json;
  size_t cap;
  size_t pos;
  off_t i;
  int first = 1;

  if(strcmp(method, MHD_HTTP_METHOD_GET)) {
    return send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method");
  }
  if(fast_resolve_paths(conn, &paths) || fast_read_map(paths.map, paths.chunk_count, &done)) {
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid upload path");
  }
  if(paths.overwrite) {
    struct stat target_stat;
    if(!lstat(paths.target, &target_stat)) {
      unlink(paths.temp);
      unlink(paths.map);
      if(done) {
        memset(done, 0, (size_t)paths.chunk_count);
      }
    }
  }
  cap = 128 + (size_t)paths.chunk_count * 24;
  json = malloc(cap);
  if(!json) {
    free(done);
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "out of memory");
  }
  pos = (size_t)snprintf(json, cap,
                         "{\"ok\":true,\"total_chunks\":%lld,\"completed\":[",
                         (long long)paths.chunk_count);
  for(i = 0; i < paths.chunk_count; i++) {
    if(done && done[i]) {
      pos += (size_t)snprintf(json + pos, cap - pos, "%s%lld", first ? "" : ",",
                              (long long)i);
      first = 0;
    }
  }
  snprintf(json + pos, cap - pos, "]}");
  free(done);
  return send_buffer(conn, MHD_HTTP_OK, json, "application/json");
}

enum MHD_Result
fast_upload_complete(struct MHD_Connection *conn, const char *method) {
  fast_upload_paths_t paths;
  unsigned char *done = NULL;
  char *base;
  char *name;
  struct stat st;
  int fd = -1;
  enum MHD_Result ret;

  if(strcmp(method, MHD_HTTP_METHOD_POST)) {
    return send_json_error(conn, MHD_HTTP_METHOD_NOT_ALLOWED, "invalid method");
  }
  if(fast_resolve_paths(conn, &paths)) {
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid upload path");
  }
  base = fs_path_value(query_value(conn, "path"));
  name = query_value(conn, "name");
  if(!base || !name || ensure_parent_dirs(base, name)) {
    free(base);
    free(name);
    return send_json_error(conn, MHD_HTTP_FORBIDDEN, "target parent is not writable");
  }
  free(base);
  free(name);
  if(paths.size == 0) {
    fd = open(paths.temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if(fd < 0) {
      return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "cannot complete upload");
    }
    if(fchmod_0777(fd) || close(fd) || rename(paths.temp, paths.target)) {
      return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "cannot complete upload");
    }
    unlink(paths.map);
    return send_json_ok(conn);
  }
  if(stat(paths.temp, &st) || st.st_size < paths.size ||
     fast_read_map(paths.map, paths.chunk_count, &done) ||
     !fast_all_done(done, paths.chunk_count)) {
    free(done);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "upload incomplete");
  }
  free(done);
  fd = open(paths.temp, O_WRONLY);
  if(fd < 0) {
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "cannot complete upload");
  }
  if(ftruncate(fd, paths.size) || fsync(fd) || fchmod_0777(fd) || close(fd)) {
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "cannot complete upload");
  }
  if(rename(paths.temp, paths.target)) {
    return send_json_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "cannot complete upload");
  }
  unlink(paths.map);
  ret = send_json_ok(conn);
  return ret;
}
