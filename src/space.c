#include "filemgr_internal.h"

#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#ifdef __SCE__
#include <sys/mount.h>
#endif

#include "json_util.h"
#include "path_util.h"
#include "smb.h"
#include "ntfs.h"
#define WFM_VFS_IMPLEMENTATION
#include "vfs.h"
#undef WFM_VFS_IMPLEMENTATION

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

static struct native_space {
  const char *label_key;
  const char *path;
  dev_t device;
  unsigned long long free_bytes;
  unsigned long long total_bytes;
  int known;
} mounts[] = {
#ifdef __SCE__
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
#else
  {"storageRoot", "/"},
#endif
};
static pthread_mutex_t native_space_lock = PTHREAD_MUTEX_INITIALIZER;

static void
append_native_spaces(strbuf_t *json, const char *current, int *first) {
  enum { count = sizeof(mounts) / sizeof(mounts[0]) };
  struct stat devices[count];
  int present[count];
  size_t selected = count;
  int native = !wfm_path_virtual(current);

  pthread_mutex_lock(&native_space_lock);
  for(size_t i = 0; i < count; i++) {
    present[i] = !stat(mounts[i].path, &devices[i]);
#ifdef __SCE__
    if(present[i]) present[i] = path_is_mounted(mounts[i].path, &devices[i]);
    if(present[i] && native && smb_path_current(current, mounts[i].path)) selected = i;
#else
    if(present[i] && native) selected = i;
#endif
  }
  /* The picker falls back to internal storage for other native directories. */
  size_t measure = selected == count && native ? 0 : selected;

  for(size_t i = 0; i < count; i++) {
    struct native_space *item = &mounts[i];
    if(!present[i]) {
      item->known = 0;
      continue;
    }
    if(item->device != devices[i].st_dev) item->known = 0;
    item->device = devices[i].st_dev;
    if(i == measure) {
      struct statvfs vfs;
      if(!statvfs(item->path, &vfs)) {
        unsigned long block_size = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
        item->free_bytes = (unsigned long long)vfs.f_bavail * block_size;
        item->total_bytes = (unsigned long long)vfs.f_blocks * block_size;
        item->known = 1;
      }
    }
    if(!*first) strbuf_append(json, ",");
    *first = 0;
    strbuf_append(json, "{\"label_key\":");
    json_escape(json, item->label_key);
    strbuf_append(json, ",\"path\":");
    json_escape(json, item->path);
    strbuf_printf(json, ",\"available\":%s,\"free\":%llu,\"total\":%llu,\"current\":%s}",
                  item->known ? "true" : "null", item->known ? item->free_bytes : 0,
                  item->known ? item->total_bytes : 0, i == selected ? "true" : "false");
  }
  pthread_mutex_unlock(&native_space_lock);
}

enum MHD_Result
api_space(struct MHD_Connection *conn) {
  char *current = fs_path_value(query_value(conn, "path"));
  const char *rescan = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "rescan");
  int refresh = rescan && !strcmp(rescan, "1");
  strbuf_t b = {0};
  int first = 1;

  strbuf_append(&b, "{\"ok\":true,\"spaces\":[");
  append_native_spaces(&b, current, &first);
  usbntfs_spaces(&b, current, &first, refresh);
  smb_append_spaces(&b, current, &first, refresh);
  free(current);
  strbuf_append(&b, "]}");
  return send_buffer(conn, MHD_HTTP_OK, b.data, "application/json");
}
