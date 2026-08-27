#pragma once

#include <microhttpd.h>

enum MHD_Result api_pkg_info(struct MHD_Connection *conn);
enum MHD_Result api_pkg_icon(struct MHD_Connection *conn);
