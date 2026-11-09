#pragma once

#include <dirent.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "smb.h"

/* FTP/WebDAV transport. SMB and native files retain their existing drivers. */
int net_path(const char *path);
int remote_path(const char *path);
int net_join(char *out, size_t size, const char *dir, const char *name);
int net_parse_address(const char *input, smb_address_t *address);
int net_probe(const smb_address_t *address, struct statvfs *space);
const char *net_error_detail(void);
const char *net_error_code(int error);
typedef struct net_dir net_dir_t;
net_dir_t *net_opendir(const char *path);
struct dirent *net_readdir(net_dir_t *dir);
void net_closedir(net_dir_t *dir);
int net_stat(const char *path, struct stat *st);
int net_statvfs(const char *path, struct statvfs *st);
int net_fd(int fd);
int net_open(const char *path, int flags);
int net_close(int fd);
int net_fstat(int fd, struct stat *st);
void net_set_size(int fd, uint64_t size);
ssize_t net_read(int fd, void *data, size_t size);
ssize_t net_pread(int fd, void *data, size_t size, off_t offset);
ssize_t net_write(int fd, const void *data, size_t size);
off_t net_lseek(int fd, off_t offset, int whence);
int net_fsync(int fd);
int net_mkdir(const char *path);
int net_remove(const char *path, int directory);
/* 1: same server/account namespace; 0: needs copy; -1: credentials unavailable. */
int net_rename_compatible(const char *source, const char *destination);
int net_rename(const char *source, const char *destination);
