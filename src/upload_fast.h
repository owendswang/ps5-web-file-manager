#pragma once

#include <stddef.h>

#include <microhttpd.h>

int fast_upload_is_chunk_request(const char *url, const char *method);
int fast_upload_begin(struct MHD_Connection *conn, void **upload_ctx);
int fast_upload_data(void *upload_ctx, const char *data, size_t size);
enum MHD_Result fast_upload_finish(struct MHD_Connection *conn,
                                   void *upload_ctx);
void fast_upload_free(void *upload_ctx);

enum MHD_Result fast_upload_status(struct MHD_Connection *conn,
                                   const char *method);
enum MHD_Result fast_upload_complete(struct MHD_Connection *conn,
                                     const char *method);
