#pragma once

#include <dirent.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "json_util.h"

/* App-local USB filesystem; these are not kernel mounts. */
int usbntfs_path(const char *path);
int usbntfs_fd(int fd);
void usbntfs_spaces(strbuf_t *json, const char *current, int *first, int rescan);
int usbntfs_present(const char *path);
void usbntfs_reap_idle(void);
const char *usbntfs_error(void);
void usbntfs_clear_error(void);
int usbntfs_stat(const char *path, struct stat *st);
int usbntfs_statvfs(const char *path, struct statvfs *st);
typedef struct usbntfs_dir usbntfs_dir_t;
usbntfs_dir_t *usbntfs_opendir(const char *path);
struct dirent *usbntfs_readdir(usbntfs_dir_t *dir);
int usbntfs_closedir(usbntfs_dir_t *dir);
int usbntfs_open(const char *path, int flags);
int usbntfs_close(int fd);
int usbntfs_fstat(int fd, struct stat *st);
ssize_t usbntfs_read(int fd, void *data, size_t size);
ssize_t usbntfs_pread(int fd, void *data, size_t size, off_t offset);
ssize_t usbntfs_write(int fd, const void *data, size_t size);
off_t usbntfs_seek(int fd, off_t offset, int whence);
int usbntfs_sync(int fd);
int usbntfs_mkdir(const char *path);
int usbntfs_remove(const char *path, int directory);
int usbntfs_rename(const char *source, const char *target);
int usbntfs_access(const char *path, int mode);
