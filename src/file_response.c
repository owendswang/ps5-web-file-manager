#include "filemgr.h"

#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "filemgr_internal.h"
#include "mime.h"
#include "websrv.h"

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
