/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <sys/mman.h>

#include "notify.h"

static int patch_restore;
static int patch_zh;
static const char *patch_url;
static int browser_patch_main(void);
static int browser_patch_detach(int pid);
static const char *patch_failure_text(const char *failure);

#if !WFM_DEBUG
static int patch_quiet_log(const char *text, ...) { (void)text; return 0; }
#define printf patch_quiet_log
#define puts patch_quiet_log
#define perror(text) ((void)(text))
#endif

/* Compile one copy for both operations, with the upstream `make test` flags. */
#define RESTORE patch_restore
#define FULLSCREEN_SAFE_SCAN 1
#define TEST_HERMES_EXPORT 0
#define TEST_HBC_EXPORT 0
#define TEST_HBC_LIVE_PLAN 1
#define EXPORT_FIXED_PROFILE 0
#define DIAG_DEEP 1
#define HYBRID_BROWSER_URL patch_url
#define main browser_patch_main
/* stdout was initialized at server startup and is shared with its threads. */
#define setvbuf(stream, buffer, mode, size) ((void)0)
#include "../.build-fullscreen/main.c"
#undef setvbuf
#undef main
#undef printf
#undef puts
#undef perror

static const char *
patch_failure_text(const char *failure) {
  static const char *const reasons[][2] = {
    {"ShellUI not found", "未找到 ShellUI"},
    {"browser engine unavailable or ambiguous", "无法确定浏览器引擎"},
    {"browser discovery or validation error", "浏览器定位或校验失败"},
    {"cannot attach to ShellUI", "无法附加到 ShellUI"},
    {"cannot stop ShellUI", "无法暂停 ShellUI"},
    {"live browser identity changed", "浏览器运行状态已改变"},
    {"unexpected browser patch bytes", "浏览器补丁位置的内容不符合预期"},
    {"patch write or verification error", "补丁写入或校验失败"},
    {"rollback verification error", "回滚校验失败"},
    {"ShellUI exited or tracing ended unexpectedly", "ShellUI 已退出或跟踪意外结束"},
    {"JSC native image or exports unavailable", "无法读取 JSC 模块或导出符号"},
    {"cannot stop ShellUI for JSC validation", "无法暂停 ShellUI 以校验 JSC"},
    {"JSC reference, INI and discovery rejected", "JSC 预设、INI 和自动定位均未通过校验"},
    {"cannot verify stopped JSC thread positions", "无法校验暂停的 JSC 线程位置"},
    {"JSC RAM write or readback error", "JSC 内存写入或回读失败"},
    {"JSC entry rollback verification error", "JSC 入口回滚校验失败"},
    {"JSC toggle rollback verification error", "JSC 开关回滚校验失败"},
    {"JSC storage rollback verification error", "JSC 数据区回滚校验失败"}
  };
  if(patch_zh) {
    for(size_t i = 0; i < sizeof(reasons) / sizeof(*reasons); i++) {
      if(!strcmp(failure, reasons[i][0])) return reasons[i][1];
    }
  }
  return failure;
}

static int
browser_patch_detach(int pid) {
  unsigned int attempts = 0;

  for(;;) {
    if(!trace(PT_DETACH, pid)) return 0;
    int error = errno, status = 0;
    pid_t child = waitpid(pid, &status, WNOHANG);
    if((child == pid && (WIFEXITED(status) || WIFSIGNALED(status))) ||
       (child < 0 && errno == ECHILD)) {
      errno = error;
      return 1;
    }
    if(++attempts <= 3 || attempts % 10 == 0) {
      DIAG_EVENT("Final detach retry=%u errno=%d child=%d status=%#x",
                 attempts, error, (int)child, status);
    }
    /* Retain the recovery worker while this process still owns the tracee. */
    usleep(attempts < 3 ? 10000 : 100000);
  }
}

/* Keep logs and discovery snapshots outside the resident ELF's BSS. */
struct patch_workspace {
  __typeof__(diagnostic) report;
  __typeof__(hybrid_record) hybrid[2];
  __typeof__(hybrid_discovered_native) native;
  __typeof__(jsc_record) jsc;
  __typeof__(scan_native_probes) probes;
  __typeof__(hbc_candidate_inventory) inventory;
  __typeof__(hmi_budget) budget;
  __typeof__(anchor_last_graph) graph;
  __typeof__(anchor_open_graph) open_graph;
  uint8_t code[2][HA_CODE_BYTES];
};

static void
patch_clear_state(void) {
  hybrid_pending = hybrid_safe_used = hybrid_discovered_valid = 0;
  hybrid_live_verified = 0;
  hybrid_cache_status = "not attempted";
  jsc_pending = 0;
  jsc_cache_status = "not attempted";
  fixed = NULL;
  hermes_base = fixed_read_bytes = fixed_read_calls = 0;
  scan_omitted_oversized_bytes = 0;
  anchor_report_code(0, NULL, NULL);
}

int
browser_patch(int restore, const char *url, int zh) {
  size_t page = (size_t)getpagesize();
  size_t size = (sizeof(struct patch_workspace) + page - 1) / page * page;
  struct patch_workspace *work = mmap(NULL, size, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANON, -1, 0);
  int result;

  if(work == MAP_FAILED) {
    notify_user(zh ? "%s失败：无法分配诊断工作区。" :
                    "Browser %s failed: cannot allocate diagnostic workspace.",
                zh ? (restore ? "还原窗口" : "全屏") :
                     (restore ? "layout restore" : "fullscreen"));
    return 1;
  }
  diagnostic_state = &work->report;
  hybrid_record_state = &work->hybrid[0];
  hybrid_disk_record_state = &work->hybrid[1];
  hybrid_discovered_native_state = &work->native;
  jsc_record_state = &work->jsc;
  scan_native_probes_state = &work->probes;
  hbc_candidate_inventory_state = &work->inventory;
  hmi_budget_state = &work->budget;
  anchor_last_graph_state = &work->graph;
  anchor_open_graph_state = &work->open_graph;
  anchor_code_state = work->code;
  patch_restore = restore;
  patch_zh = zh;
  patch_url = url;
  patch_clear_state();
  result = browser_patch_main();
  patch_clear_state();
  diagnostic_state = NULL;
  hybrid_record_state = hybrid_disk_record_state = NULL;
  hybrid_discovered_native_state = NULL;
  jsc_record_state = NULL;
  scan_native_probes_state = NULL;
  hbc_candidate_inventory_state = NULL;
  hmi_budget_state = NULL;
  anchor_last_graph_state = NULL;
  anchor_open_graph_state = NULL;
  anchor_code_state = NULL;
  patch_url = NULL;
  patch_restore = 0;
  patch_zh = 0;
  if(munmap(work, size)) {
    notify_user(zh ? "浏览器布局诊断工作区释放失败。" :
                    "Browser layout diagnostic workspace cleanup failed.");
  }
#if WFM_DEBUG
  else printf("Browser patch workspace released: %zu bytes\n", size);
#endif
  return result;
}
