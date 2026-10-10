#pragma once

#include <stddef.h>

typedef struct ultrapack_helper_callbacks {
  int (*cancel_requested)(void *arg);
  void (*progress)(void *arg, unsigned long long done, unsigned long long total);
  void (*current_file)(void *arg, const char *path);
  void *arg;
} ultrapack_helper_callbacks_t;

typedef struct ultrapack_helper_result {
  char code[64];
  char message[160];
} ultrapack_helper_result_t;

int ultrapack_helper_probe(void);
int ultrapack_helper_autostart(void);
int ultrapack_helper_convert(unsigned long job_id, const char *source,
                             const char *destination, const char *format,
                             const ultrapack_helper_callbacks_t *callbacks,
                             ultrapack_helper_result_t *result);
