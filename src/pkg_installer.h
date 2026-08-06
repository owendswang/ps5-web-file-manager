#pragma once

#define PKG_INSTALL_UNSUPPORTED 0x7fffffff

int pkg_installer_initialize(void);
int pkg_installer_install(const char *path);
