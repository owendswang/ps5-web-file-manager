#pragma once

#define PKG_INSTALL_UNSUPPORTED 0x7fffffff

int pkg_installer_initialize(void);
int pkg_installer_install(const char *path);

/* Opens the original package for one HTTP response. The caller owns the fd. */
struct stat;
int pkg_installer_source_open(const char *name, struct stat *info);
