#include "ultrapack_helper.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define WUP_HEADER_MAGIC "WUPH"
#define WUP_HEADER_SIZE 20U
#define WUP_MAX_PAYLOAD (1024U * 1024U)
#define WUP_HELPER_ELF "/data/wfm/wfm-ultrapack-helper.elf"
#define WUP_ELFLDR_PORT 9021

#ifdef __linux__
#define WUP_HELPER_SOCKET "/tmp/wfm-ultrapack-helper.sock"
#else
#define WUP_HELPER_SOCKET "/system_tmp/wfm-ultrapack-helper.sock"
#endif

#define WUP_MSG_PING             1U
#define WUP_MSG_PONG             2U
#define WUP_MSG_CONVERT          10U
#define WUP_MSG_CANCEL           11U
#define WUP_MSG_STATUS           12U

#define WUP_MSG_ACCEPTED         20U
#define WUP_MSG_PROGRESS         21U
#define WUP_MSG_CURRENT_FILE     22U
#define WUP_MSG_DONE             24U
#define WUP_MSG_ERROR            25U

typedef struct helper_frame {
  uint16_t type;
  uint16_t flags;
  uint32_t payload_size;
  uint64_t request_id;
} helper_frame_t;

static uint16_t get16(const unsigned char *p) {
  return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t get32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t get64(const unsigned char *p) {
  return ((uint64_t)get32(p) << 32) | get32(p + 4);
}

static void put16(unsigned char *p, uint16_t value) {
  p[0] = (unsigned char)(value >> 8);
  p[1] = (unsigned char)value;
}

static void put32(unsigned char *p, uint32_t value) {
  p[0] = (unsigned char)(value >> 24);
  p[1] = (unsigned char)(value >> 16);
  p[2] = (unsigned char)(value >> 8);
  p[3] = (unsigned char)value;
}

static void put64(unsigned char *p, uint64_t value) {
  put32(p, (uint32_t)(value >> 32));
  put32(p + 4, (uint32_t)value);
}

static int send_all(int fd, const void *data, size_t size) {
  const unsigned char *p = data;
  size_t done = 0;
  while(done < size) {
    ssize_t n = send(fd, p + done, size - done, 0);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) return -1;
    done += (size_t)n;
  }
  return 0;
}

static int recv_all(int fd, void *data, size_t size) {
  unsigned char *p = data;
  size_t done = 0;
  while(done < size) {
    ssize_t n = recv(fd, p + done, size - done, 0);
    if(n < 0 && errno == EINTR) continue;
    if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -2;
    if(n < 0) return -1;
    if(!n) return 0;
    done += (size_t)n;
  }
  return 1;
}

static int send_frame(int fd, uint16_t type, uint64_t request_id,
                      const void *payload, uint32_t payload_size) {
  unsigned char header[WUP_HEADER_SIZE];
  if(payload_size > WUP_MAX_PAYLOAD || (payload_size && !payload)) {
    errno = EINVAL;
    return -1;
  }
  memcpy(header, WUP_HEADER_MAGIC, 4);
  put16(header + 4, type);
  put16(header + 6, 0);
  put32(header + 8, payload_size);
  put64(header + 12, request_id);
  if(send_all(fd, header, sizeof(header))) return -1;
  return payload_size ? send_all(fd, payload, payload_size) : 0;
}

static int recv_frame(int fd, helper_frame_t *frame, char *payload, size_t capacity) {
  unsigned char header[WUP_HEADER_SIZE];
  int ret = recv_all(fd, header, sizeof(header));
  if(ret <= 0) return ret;
  if(memcmp(header, WUP_HEADER_MAGIC, 4)) {
    errno = EPROTO;
    return -1;
  }
  frame->type = get16(header + 4);
  frame->flags = get16(header + 6);
  frame->payload_size = get32(header + 8);
  frame->request_id = get64(header + 12);
  if(frame->payload_size > WUP_MAX_PAYLOAD || frame->payload_size >= capacity) {
    errno = EMSGSIZE;
    return -1;
  }
  if(frame->payload_size) {
    ret = recv_all(fd, payload, frame->payload_size);
    if(ret <= 0) return ret;
  }
  payload[frame->payload_size] = 0;
  return 1;
}

static int connect_helper(void) {
  struct sockaddr_un addr;
  struct timeval timeout = {0, 250000};
  const char *path = WUP_HELPER_SOCKET;
  size_t path_len = strlen(path);
  socklen_t addr_len;
  int fd;

  if(path_len >= sizeof(addr.sun_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) return -1;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, path, path_len + 1);
#ifndef __linux__
  addr.sun_len = (unsigned char)SUN_LEN(&addr);
  addr_len = addr.sun_len;
#else
  addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1);
#endif
  if(connect(fd, (struct sockaddr *)&addr, addr_len)) {
    int error = errno;
    close(fd);
    errno = error;
    return -1;
  }
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  return fd;
}

static int wait_readable(int fd, int timeout_ms) {
  struct pollfd item;
  int ret;
  memset(&item, 0, sizeof(item));
  item.fd = fd;
  item.events = POLLIN;
  do {
    ret = poll(&item, 1, timeout_ms);
  } while(ret < 0 && errno == EINTR);
  if(ret <= 0) return ret;
  return item.revents & (POLLIN | POLLHUP | POLLERR) ? 1 : -1;
}

static int ping_helper(int fd) {
  helper_frame_t frame;
  char payload[1024];
  int ret;
  if(send_frame(fd, WUP_MSG_PING, 0, NULL, 0) || wait_readable(fd, 1000) != 1) return -1;
  ret = recv_frame(fd, &frame, payload, sizeof(payload));
  return ret == 1 && frame.type == WUP_MSG_PONG ? 0 : -1;
}

int ultrapack_helper_probe(void) {
  int fd = connect_helper();
  if(fd < 0) return -1;
  int ret = ping_helper(fd);
  close(fd);
  return ret;
}

int ultrapack_helper_autostart(void) {
  if(!ultrapack_helper_probe()) return 0;
#ifndef __linux__
  /* Auto-load helper binary if present */
  struct stat st;
  if(!stat(WUP_HELPER_ELF, &st) && S_ISREG(st.st_mode)) {
    /* Send to elfldr */
  }
#endif
  return ultrapack_helper_probe();
}

int ultrapack_helper_convert(unsigned long job_id, const char *source,
                             const char *destination, const char *format,
                             const ultrapack_helper_callbacks_t *callbacks,
                             ultrapack_helper_result_t *result) {
  if(result) memset(result, 0, sizeof(*result));
  int fd = connect_helper();
  if(fd < 0) {
    if(result) {
      snprintf(result->code, sizeof(result->code), "ultrapack_helper_not_running");
      snprintf(result->message, sizeof(result->message), "UltraPack helper is not running");
    }
    return -1;
  }

  char request[PATH_MAX * 2 + 128];
  snprintf(request, sizeof(request), "%s\n%s\n%s", source, destination, format ? format : "ffpfsc");
  if(send_frame(fd, WUP_MSG_CONVERT, job_id, request, (uint32_t)strlen(request))) {
    if(result) {
      snprintf(result->code, sizeof(result->code), "ultrapack_send_failed");
      snprintf(result->message, sizeof(result->message), "failed to send conversion request to helper");
    }
    close(fd);
    return -1;
  }

  helper_frame_t frame;
  char payload[4096];

  while(1) {
    if(callbacks && callbacks->cancel_requested && callbacks->cancel_requested(callbacks->arg)) {
      send_frame(fd, WUP_MSG_CANCEL, job_id, NULL, 0);
    }
    int readable = wait_readable(fd, 250);
    if(readable < 0) break;
    if(readable == 0) continue;

    int ret = recv_frame(fd, &frame, payload, sizeof(payload));
    if(ret <= 0) break;

    if(frame.type == WUP_MSG_PROGRESS && frame.payload_size >= 16) {
      uint64_t total = get64((const unsigned char *)payload);
      uint64_t done = get64((const unsigned char *)payload + 8);
      if(callbacks && callbacks->progress) {
        callbacks->progress(callbacks->arg, done, total);
      }
    } else if(frame.type == WUP_MSG_CURRENT_FILE) {
      if(callbacks && callbacks->current_file) {
        callbacks->current_file(callbacks->arg, payload);
      }
    } else if(frame.type == WUP_MSG_DONE) {
      close(fd);
      return 0;
    } else if(frame.type == WUP_MSG_ERROR) {
      if(result) {
        snprintf(result->code, sizeof(result->code), "convert_failed");
        snprintf(result->message, sizeof(result->message), "%s", payload);
      }
      close(fd);
      return -1;
    }
  }

  if(result && !result->code[0]) {
    snprintf(result->code, sizeof(result->code), "ultrapack_helper_disconnected");
    snprintf(result->message, sizeof(result->message), "connection to UltraPack helper was lost");
  }
  close(fd);
  return -1;
}
