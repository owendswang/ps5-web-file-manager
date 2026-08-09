#include "pkg_installer.h"

#ifndef __linux__

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

typedef struct pkg_metadata {
  const char *uri;
  const char *ex_uri;
  const char *playgo_scenario_id;
  const char *content_id;
  const char *content_name;
  const char *icon_url;
} pkg_metadata_t;

typedef struct pkg_info {
  char content_id[48];
  int type;
  int platform;
} pkg_info_t;

typedef struct playgo_info {
  char lang[8][30];
  char scenario_ids[3][64];
  char content_ids[64];
  long unknown[810];
} playgo_info_t;

int sceAppInstUtilInitialize(void);
int sceAppInstUtilInstallByPackage(const pkg_metadata_t *, pkg_info_t *,
                                   playgo_info_t *);

static pthread_mutex_t installer_lock = PTHREAD_MUTEX_INITIALIZER;
static int installer_initialized;

static int
initialize_locked(void) {
  int result;

  if(!installer_initialized) {
    result = sceAppInstUtilInitialize();
    if(result) return result;
    installer_initialized = 1;
  }
  return 0;
}

int
pkg_installer_initialize(void) {
  int result;

  pthread_mutex_lock(&installer_lock);
  result = initialize_locked();
  pthread_mutex_unlock(&installer_lock);
  return result;
}

int
pkg_installer_install(const char *path) {
  char install_path[PATH_MAX + sizeof("/user")];
  const char *uri = path;
  pkg_metadata_t metadata = {
    .uri = NULL,
    .ex_uri = "",
    .playgo_scenario_id = "",
    .content_id = "",
    .content_name = "",
    .icon_url = ""
  };
  pkg_info_t pkg_info = {0};
  playgo_info_t playgo_info = {0};
  int result;

  if(!path) return -1;
  if(!strncmp(path, "/data/", 6)) {
    snprintf(install_path, sizeof(install_path), "/user%s", path);
    uri = install_path;
  }
  metadata.uri = uri;

  pthread_mutex_lock(&installer_lock);
  result = initialize_locked();
  if(result) {
    pthread_mutex_unlock(&installer_lock);
    return result;
  }
  result = sceAppInstUtilInstallByPackage(&metadata, &pkg_info, &playgo_info);
  pthread_mutex_unlock(&installer_lock);
  return result;
}

#else

int
pkg_installer_initialize(void) {
  return PKG_INSTALL_UNSUPPORTED;
}

int
pkg_installer_install(const char *path) {
  (void)path;
  return PKG_INSTALL_UNSUPPORTED;
}

#endif
