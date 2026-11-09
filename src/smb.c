#include "smb.h"
#include "remote.h"
#include "filemgr_internal.h"
#include "path_util.h"
#include "transfer.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <smb2/libsmb2-dcerpc-srvsvc.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/smb2-errors.h>

#define SMB_CONNECTION_MAX 64
#if WFM_DEBUG
#define SMB_DEBUG(...) do { fprintf(stderr, "SMB "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while(0)
#else
#define SMB_DEBUG(...) do {} while(0)
#endif
typedef struct saved_smb {
  smb_address_t address;
  time_t checked_at;
  int available;
  int authenticated;
  unsigned long long free_bytes;
  unsigned long long total_bytes;
} saved_smb_t;

static saved_smb_t connections[SMB_CONNECTION_MAX];
static size_t connection_count;
static pthread_once_t config_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;
static int config_error;
static char client_error[512];
static uint32_t client_status;
static int refreshing_spaces;
enum { CONFIG_MIGRATE_SMB = 1, CONFIG_MIGRATE_CUSTOM = 2 };

int smb_path(const char *path) { return path && !strncmp(path, "smb://", 6); }
void smb_lock(void) { pthread_mutex_lock(&client_mutex); }
void smb_unlock(void) { pthread_mutex_unlock(&client_mutex); }
int smb_result(int result) {
  if(result < 0) { errno = -result; return -1; }
  return result;
}
int smb_path_current(const char *current, const char *root) {
  size_t len = strlen(root);
  return current && !strncmp(current, root, len) &&
    (!current[len] || current[len] == '/');
}

static size_t connection_index(const char *path) {
  size_t index;
  for(index = 0; index < connection_count; index++)
    if(!strcmp(connections[index].address.path, path)) break;
  return index;
}

static int copy_field(char *out, size_t size, const char *in, size_t len) {
  if(len >= size) { errno = ENAMETOOLONG; return -1; }
  memcpy(out, in, len);
  out[len] = 0;
  return 0;
}

static int decode_credential(char *value) {
  char *out = value;
  for(char *p = value; *p; p++) {
    if(*p == '%') {
      unsigned int byte;
      if(!p[1] || !p[2] || !isxdigit((unsigned char)p[1]) ||
         !isxdigit((unsigned char)p[2]) || sscanf(p + 1, "%2x", &byte) != 1 ||
         !byte || byte < 32 || byte == 127) { errno = EINVAL; return -1; }
      *out++ = (char)byte;
      p += 2;
    } else *out++ = *p;
  }
  *out = 0;
  return 0;
}

int smb_parse_address(const char *input, smb_address_t *a) {
  const char *authority, *slash, *server, *at, *share, *end;
  memset(a, 0, sizeof(*a));
  if(!smb_path(input) || strlen(input) >= PATH_MAX) goto invalid;
  for(const unsigned char *p = (const unsigned char *)input; *p; p++)
    if(*p < 32 || *p == 127 || *p == '\\') goto invalid;
  authority = input + 6;
  slash = strchr(authority, '/');
  if(!slash || slash == authority || !slash[1]) goto invalid;
  at = memchr(authority, '@', (size_t)(slash - authority));
  server = authority;
  if(at) {
    const char *colon = memchr(authority, ':', (size_t)(at - authority));
    const char *user_end = colon ? colon : at;
    const char *semicolon = memchr(authority, ';', (size_t)(user_end - authority));
    const char *user = authority;
    if(semicolon) {
      if(copy_field(a->domain, sizeof(a->domain), authority, (size_t)(semicolon - authority))) return -1;
      user = semicolon + 1;
    }
    if(user == user_end || copy_field(a->user, sizeof(a->user), user, (size_t)(user_end - user))) goto invalid;
    if(colon && copy_field(a->password, sizeof(a->password), colon + 1, (size_t)(at - colon - 1))) return -1;
    if(decode_credential(a->user) || decode_credential(a->password) || decode_credential(a->domain)) return -1;
    server = at + 1;
  } else strcpy(a->user, "Guest");
  if(server == slash || copy_field(a->server, sizeof(a->server), server, (size_t)(slash - server))) goto invalid;
  for(char *p = a->server; *p; p++) {
    if(isspace((unsigned char)*p) || *p == '@' || *p == '?' || *p == '#') goto invalid;
    *p = (char)tolower((unsigned char)*p);
  }
  share = slash + 1;
  end = strchr(share, '/');
  if(!end) end = share + strlen(share);
  if(end == share || copy_field(a->share, sizeof(a->share), share, (size_t)(end - share))) goto invalid;
  if(strchr(a->share, '?') || strchr(a->share, '#') ||
     !strcmp(a->share, ".") || !strcmp(a->share, "..")) goto invalid;
  if(*end && copy_field(a->relative, sizeof(a->relative), end + 1, strlen(end + 1))) return -1;
  size_t len = strlen(a->relative);
  while(len && a->relative[len - 1] == '/') a->relative[--len] = 0;
  if(a->relative[0] && !relative_path_safe(a->relative)) goto invalid;
  int n = snprintf(a->path, sizeof(a->path), "smb://%s/%s%s%s", a->server,
                   a->share, a->relative[0] ? "/" : "", a->relative);
  if(n < 0 || (size_t)n >= sizeof(a->path)) { errno = ENAMETOOLONG; return -1; }
  return 0;
invalid:
  errno = EINVAL;
  return -1;
}

static const char *config_dir(void) {
  const char *override = getenv("WFM_CONFIG_DIR");
  return override && override[0] ? override : "/data/wfm/config";
}

static int save_config(int local);

static void trim_local_path(char *path) {
  size_t len = strlen(path);
  while(len > 1 && path[len - 1] == '/') path[--len] = 0;
}

static int builtin_local_path(const char *path) {
  return !strcmp(path, "/data") ||
    (!strncmp(path, "/mnt/", 5) && path[5] && !strchr(path + 5, '/'));
}

static int parse_saved_address(const char *path, smb_address_t *a) {
  if(net_path(path)) return net_parse_address(path,a);
  if(path[0] != '/') return smb_parse_address(path, a);
  memset(a, 0, sizeof(*a));
  if(copy_field(a->path, sizeof(a->path), path, strlen(path))) return -1;
  trim_local_path(a->path);
  return 0;
}

static int config_kind(const char *path) {
  return path[0]=='/'?1:!net_path(path)?0:
    !strncmp(path,"ftp://",6)?2:3;
}

static void load_config_file(const char *name, int kind, int *migrate) {
  char path[PATH_MAX], line[PATH_MAX + 1100];
  if(snprintf(path, sizeof(path), "%s/%s", config_dir(), name) >= (int)sizeof(path)) { config_error = ENAMETOOLONG; return; }
  FILE *file = fopen(path, "rb");
  if(!file) { if(errno != ENOENT) config_error = errno; return; }
  while(fgets(line, sizeof(line), file)) {
    char *p = line, *fields[3];
    line[strcspn(line, "\r\n")] = 0;
    if(!line[0] || line[0] == '#') continue;
    for(size_t i = 0; i < 3; i++) fields[i] = strsep(&p, "\t");
    /* Ignore saved TLS connections without downgrading them or blocking other paths. */
    if((kind==2 || kind==3) && (!strncmp(fields[0],"ftps://",7) ||
       !strncmp(fields[0],"webdavs://",10) || !strncmp(fields[0],"davs://",7) ||
       !strncmp(fields[0],"https://",8))) continue;
    int local = fields[0][0] == '/';
    if(p) *migrate |= CONFIG_MIGRATE_SMB; /* Discard passwords from the old four-column format. */
    /* Split local paths saved by the interim mixed format. */
    if(local && kind==0) *migrate |= CONFIG_MIGRATE_SMB | CONFIG_MIGRATE_CUSTOM;
    smb_address_t a;
    if((kind==1 && !local) || (!local && (config_kind(fields[0])!=kind || !fields[2])) || (p && strchr(p, '\t')) ||
       (local && ((fields[1] && fields[1][0]) || (fields[2] && fields[2][0]))) ||
       parse_saved_address(fields[0], &a) ||
       (!local && (copy_field(a.user, sizeof(a.user), fields[1], strlen(fields[1])) ||
                   copy_field(a.domain, sizeof(a.domain), fields[2], strlen(fields[2]))))) {
      config_error = EINVAL; break;
    }
    if(local && strcmp(fields[0], a.path)) *migrate |= CONFIG_MIGRATE_CUSTOM;
    if(local && builtin_local_path(a.path)) { *migrate |= CONFIG_MIGRATE_CUSTOM; continue; }
    memset(a.password, 0, sizeof(a.password));
    if(connection_index(a.path) < connection_count) {
      if(local) *migrate |= CONFIG_MIGRATE_CUSTOM;
      continue;
    }
    if(connection_count == SMB_CONNECTION_MAX) { config_error = EINVAL; break; }
    connections[connection_count++] = (saved_smb_t){.address = a, .available = -1};
  }
  if(ferror(file)) config_error = EIO;
  fclose(file);
  memset(line, 0, sizeof(line));
}

static void load_config(void) {
  int migrate = 0;
  load_config_file("smb.conf", 0, &migrate);
  if(!config_error) load_config_file("ftp.conf", 2, &migrate);
  if(!config_error) load_config_file("webdav.conf", 3, &migrate);
  if(!config_error) load_config_file("custom.conf", 1, &migrate);
  /* Write migrated local paths first; leave smb.conf intact if that fails. */
  if(!config_error && (migrate & CONFIG_MIGRATE_CUSTOM) && save_config(1)) config_error = errno;
  if(!config_error && (migrate & CONFIG_MIGRATE_SMB) && save_config(0)) config_error = errno;
}

static int ensure_config_dir(void) {
  char dir[PATH_MAX];
  if(copy_field(dir, sizeof(dir), config_dir(), strlen(config_dir()))) return -1;
  for(char *p = dir + 1; ; p++) {
    if(*p && *p != '/') continue;
    char saved = *p;
    *p = 0;
    if(mkdir(dir, 0700) && errno != EEXIST) return -1;
    *p = saved;
    if(!saved) break;
  }
  return 0;
}

static int save_config(int kind) {
  char path[PATH_MAX], temp[PATH_MAX];
  static const char *names[]={"smb.conf","custom.conf","ftp.conf","webdav.conf"};
  const char *name=names[kind];
  int local=kind==1;
  if(ensure_config_dir()) return -1;
  if(snprintf(path, sizeof(path), "%s/%s", config_dir(), name) >= (int)sizeof(path) ||
     snprintf(temp, sizeof(temp), "%s/%s.tmp", config_dir(), name) >= (int)sizeof(temp)) {
    errno = ENAMETOOLONG; return -1;
  }
  int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if(fd < 0) return -1;
  FILE *file = fdopen(fd, "wb");
  if(!file) { close(fd); unlink(temp); return -1; }
  int result = fchmod(fd, 0600);
  for(size_t i = 0; !result && i < connection_count; i++) {
    smb_address_t *a = &connections[i].address;
    if(config_kind(a->path)!=kind) continue;
    if((local ? fprintf(file, "%s\n", a->path) :
                fprintf(file, "%s\t%s\t%s\n", a->path, a->user, a->domain)) < 0) result = -1;
  }
  if(fflush(file) || fsync(fd)) result = -1;
  int error = errno;
  if(fclose(file) && !result) { result = -1; error = errno; }
  if(!result && rename(temp, path)) { result = -1; error = errno; }
  if(result) unlink(temp);
  errno = error;
  return result;
}

static struct smb2_context *connect_address(const smb_address_t *a) {
  client_error[0] = 0;
  client_status = 0;
  char first_error[240] = {0};
  /* Match websrv: empty-password NTLM first, then an anonymous session.
     NULL and "" select different NTLM messages in libsmb2. */
  for(int attempt = 0; attempt < (a->password[0] ? 1 : 2); attempt++) {
    struct smb2_context *ctx = smb2_init_context();
    if(!ctx) { errno = ENOMEM; return NULL; }
    smb2_set_timeout(ctx, 5);
    smb2_set_user(ctx, a->user);
    smb2_set_password(ctx, attempt ? NULL : a->password);
    smb2_set_security_mode(ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);
    if(a->domain[0]) smb2_set_domain(ctx, a->domain);
    SMB_DEBUG("connect server=%s share=%s user=%s domain=%s auth=%s password=%s signing=enabled timeout=5s",
              a->server, a->share, a->user, a->domain[0] ? a->domain : "(default)",
              attempt ? "anonymous" : "NTLM", a->password[0] ? "present" : "empty");
    int result = smb2_connect_share(ctx, a->server, a->share, a->user);
    if(result >= 0) {
      int fd = smb2_get_fd(ctx);
      transfer_socket_option(fd, SOL_SOCKET, SO_RCVBUF, TRANSFER_SOCKET_BUFFER_SIZE, "SMB connection", "SO_RCVBUF");
      transfer_socket_option(fd, SOL_SOCKET, SO_SNDBUF, TRANSFER_SOCKET_BUFFER_SIZE, "SMB connection", "SO_SNDBUF");
      SMB_DEBUG("connected server=%s share=%s auth=%s dialect=0x%04x",
                a->server, a->share, attempt ? "anonymous" : "NTLM", smb2_get_dialect(ctx));
      SMB_DEBUG("limits read=%u write=%u bytes", smb2_get_max_read_size(ctx), smb2_get_max_write_size(ctx));
      return ctx;
    }
    SMB_DEBUG("connect failed server=%s share=%s auth=%s rc=%d errno=%d ntstatus=0x%08x dialect=0x%04x detail=%s",
              a->server, a->share, attempt ? "anonymous" : "NTLM", result, -result,
              (unsigned int)smb2_get_nterror(ctx), smb2_get_dialect(ctx), smb2_get_error(ctx));
    if(!a->password[0] && !attempt) {
      snprintf(first_error, sizeof(first_error), "%.239s", smb2_get_error(ctx));
      SMB_DEBUG("retrying empty-password connection as anonymous server=%s share=%s", a->server, a->share);
    } else if(attempt) {
      snprintf(client_error, sizeof(client_error), "empty-password: %.220s; anonymous: %.220s",
               first_error, smb2_get_error(ctx));
    } else {
      snprintf(client_error, sizeof(client_error), "%s", smb2_get_error(ctx));
    }
    uint32_t status = smb2_get_nterror(ctx);
    client_status = status;
    int auth_error = status == SMB2_STATUS_LOGON_FAILURE ||
      status == SMB2_STATUS_WRONG_PASSWORD || status == SMB2_STATUS_ACCESS_DENIED;
    smb2_destroy_context(ctx);
    errno = auth_error ? EACCES : -result;
  }
  return NULL;
}

struct smb2_context *smb_connect_path(const char *path, smb_address_t *a) {
  if(smb_parse_address(path, a)) return NULL;
  pthread_once(&config_once, load_config);
  pthread_mutex_lock(&config_mutex);
  size_t matched = 0;
  for(size_t i = 0; i < connection_count; i++) {
    smb_address_t *saved = &connections[i].address;
    size_t len = strlen(saved->path);
    if(len >= matched && smb_path_current(a->path, saved->path)) {
      strcpy(a->user, saved->user);
      strcpy(a->domain, saved->domain);
      strcpy(a->password, saved->password);
      matched = len;
    }
  }
  pthread_mutex_unlock(&config_mutex);
  if(!matched) { errno = ENOENT; return NULL; }
  return connect_address(a);
}

int storage_remote_address(const char *path,smb_address_t *address) {
  pthread_once(&config_once,load_config);
  if(config_error) { errno=config_error; return -1; }
  pthread_mutex_lock(&config_mutex);
  size_t found=connection_count,length=0;
  for(size_t i=0;i<connection_count;i++) {
    const char *root=connections[i].address.path;
    if(strlen(root)>length && smb_path_current(path,root)) { found=i; length=strlen(root); }
  }
  int result=-1;
  if(found<connection_count) {
    *address=connections[found].address;
    result=0;
    if(!connections[found].authenticated && address->user[0] &&
       strcasecmp(address->user,"anonymous") && strcasecmp(address->user,"Guest")) { errno=EACCES; result=-1; }
  } else errno=ENOENT;
  pthread_mutex_unlock(&config_mutex); return result;
}

typedef struct share_enum_result {
  int done;
  int status;
  struct srvsvc_NetrShareEnum_rep *reply;
} share_enum_result_t;

static void share_enum_cb(struct smb2_context *ctx, int status, void *data, void *opaque) {
  share_enum_result_t *result = opaque;
  result->status = status;
  result->reply = data;
  result->done = 1;
}

static const char *path_error_code(int error) {
  switch(error) {
    case ENOENT: return "storage_path_not_found";
    case ENOTDIR: return "storage_path_not_directory";
    case EACCES: case EPERM: return "storage_path_denied";
    case ENAMETOOLONG: return "storage_path_too_long";
    case ELOOP: return "storage_path_loop";
    case EINVAL: return "storage_path_invalid";
    default: return "storage_path_unavailable";
  }
}

static const char *smb_error_code(int error, uint32_t status, const char *detail, const char *fallback) {
  switch(status) {
    case SMB2_STATUS_BAD_NETWORK_NAME: return "smb_share_not_found";
    case SMB2_STATUS_NO_SUCH_FILE:
    case SMB2_STATUS_OBJECT_NAME_NOT_FOUND:
    case SMB2_STATUS_OBJECT_PATH_NOT_FOUND: return "storage_path_not_found";
    case SMB2_STATUS_NOT_A_DIRECTORY: return "storage_path_not_directory";
    case SMB2_STATUS_ACCESS_DENIED:
    case SMB2_STATUS_NETWORK_ACCESS_DENIED: return "storage_path_denied";
    case SMB2_STATUS_NOT_SUPPORTED:
    case SMB2_STATUS_INVALID_PARAMETER: return "smb_protocol_error";
    case SMB2_STATUS_IO_TIMEOUT: return "smb_timeout";
  }
  if(strstr(detail, "Can not resolve into IPv4/v6")) return "smb_host_not_found";
  if(strstr(detail, "POLLHUP")) return "smb_connection_closed";
  if(strstr(detail, "Timeout expired") || strstr(detail, "timed out")) return "smb_timeout";
  /* The pinned libsmb2 sync wrapper returns -1 for socket failures; its detail retains the OS errno. */
  const char *socket_error = strstr(detail, "Socket connect failed with ");
  if(socket_error) sscanf(socket_error, "Socket connect failed with %d", &error);
  switch(error) {
    case ETIMEDOUT: return "smb_timeout";
    case ECONNREFUSED: return "smb_connection_refused";
    case ECONNRESET: case ECONNABORTED: case ENETRESET: return "smb_connection_closed";
    case EHOSTUNREACH: case ENETUNREACH: case EHOSTDOWN: return "smb_unreachable";
    case ENOENT: case ENOTDIR: case EACCES: return path_error_code(error);
    default: return fallback;
  }
}

enum MHD_Result api_smb_shares(struct MHD_Connection *conn, const char *body, size_t size) {
  char *input = body_form_value(body, size, "address");
  char *password = body_form_value(body, size, "password");
  char ipc[PATH_MAX];
  smb_address_t a;
  size_t len = input ? strlen(input) : 0;
  if(len && input[len - 1] == '/') input[--len] = 0;
  int valid = input && smb_path(input) && !strchr(input + 6, '/') &&
    snprintf(ipc, sizeof(ipc), "%s/IPC$", input) < (int)sizeof(ipc) &&
    !smb_parse_address(ipc, &a);
  free(input);
  if(!valid || (password && copy_field(a.password, sizeof(a.password), password, strlen(password)))) {
    free(password);
    return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST, "invalid SMB server address or credentials", "storage_path_invalid", NULL);
  }
  free(password);
  if(has_active_task()) return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  smb_lock();
  struct smb2_context *ctx = connect_address(&a);
  share_enum_result_t result = {0};
  int status = ctx ? smb2_share_enum_async(ctx, SHARE_INFO_1, share_enum_cb, &result) : -errno;
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  while(ctx && !status && !result.done) {
    struct pollfd pfd = {.fd = smb2_get_fd(ctx), .events = smb2_which_events(ctx)};
    int ready = poll(&pfd, 1, 1000);
    if(ready < 0 && errno == EINTR) continue;
    if(ready < 0) { status = -errno; break; }
    status = smb2_service(ctx, pfd.revents);
    clock_gettime(CLOCK_MONOTONIC, &now);
    if(!result.done && now.tv_sec - start.tv_sec >= 15) status = -ETIMEDOUT;
  }
  if(!status) status = result.status;
  if(!status && !result.reply) status = -EIO;
  strbuf_t json = {0};
  if(!status) {
    strbuf_append(&json, "{\"ok\":true,\"shares\":[");
    struct srvsvc_SHARE_INFO_1_CONTAINER *shares = &result.reply->ses.ShareInfo.Level1;
    int first = 1;
    for(uint32_t i = 0; shares->Buffer && i < shares->EntriesRead; i++) {
      struct srvsvc_SHARE_INFO_1 *share = &shares->Buffer->share_info_1[i];
      if((share->type & 3) != SHARE_TYPE_DISKTREE || !share->netname.utf8) continue;
      if(!first) strbuf_append(&json, ",");
      first = 0;
      json_escape(&json, share->netname.utf8);
    }
    strbuf_append(&json, "]}");
  }
  char detail[512];
  snprintf(detail, sizeof(detail), "%s", ctx ? smb2_get_error(ctx) : client_error);
  if(status && !detail[0]) snprintf(detail, sizeof(detail), "share enumeration failed (%d)", status);
  SMB_DEBUG("share enumeration server=%s status=%d detail=%s", a.server, status, detail);
  int password_required = status == -EACCES || status == 5;
  const char *code = password_required ? "smb_password_required" :
    smb_error_code(-status, ctx ? smb2_get_nterror(ctx) : client_status, detail, "smb_share_enum_failed");
  if(result.reply) smb2_free_data(ctx, result.reply);
  /* Keep callback state alive until outstanding requests have been canceled. */
  if(ctx) smb2_destroy_context(ctx);
  smb_unlock();
  if(status) return send_json_error_detail(conn, MHD_HTTP_BAD_GATEWAY, "SMB share enumeration failed",
    code, detail);
  return send_buffer(conn, MHD_HTTP_OK, json.data, "application/json");
}

static void local_space(saved_smb_t *saved) {
  struct stat st;
  struct statvfs vfs;
  saved->available = stat(saved->address.path, &st) || !S_ISDIR(st.st_mode) ? 0 :
    statvfs(saved->address.path, &vfs) ? -1 : 1;
  if(saved->available == 1) {
    unsigned long long block = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    saved->free_bytes = vfs.f_bavail * block;
    saved->total_bytes = vfs.f_blocks * block;
  }
}

enum MHD_Result api_smb_add(struct MHD_Connection *conn, const char *body, size_t size, int connecting) {
  char *input = body_form_value(body, size, "address");
  if(connecting) input = fs_path_value(input);
  char *password = body_form_value(body, size, "password");
  smb_address_t a = {0};
  int local = !connecting && input && input[0] == '/';
  int invalid;
  if(!input) { errno = EINVAL; invalid = 1; }
  else if(local) {
    invalid = !realpath(input, a.path);
    if(!invalid && strpbrk(a.path, "\r\n\t")) { errno = EINVAL; invalid = 1; }
  } else invalid = net_path(input)?net_parse_address(input,&a):smb_parse_address(input, &a);
  if(invalid) {
    int error = errno;
    free(input); free(password);
    return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST, strerror(error), path_error_code(error), strerror(error));
  }
  free(input);
  if(local) {
    trim_local_path(a.path);
    if(builtin_local_path(a.path)) {
      free(password);
      return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST, "path reserved for built-in storage", "storage_path_builtin", NULL);
    }
  }
  pthread_once(&config_once, load_config);
  if(connecting) {
    pthread_mutex_lock(&config_mutex);
    size_t index = connection_index(a.path);
    int found = index < connection_count;
    if(found) {
      strcpy(a.user, connections[index].address.user);
      strcpy(a.domain, connections[index].address.domain);
    }
    pthread_mutex_unlock(&config_mutex);
    if(!found) { free(password); return send_json_error_detail(conn, MHD_HTTP_NOT_FOUND, "Remote connection not found", "smb_connection_not_found", NULL); }
  }
  if(!local && password && copy_field(a.password, sizeof(a.password), password, strlen(password))) {
    free(password); return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST, "invalid remote password", "smb_password_invalid", NULL);
  }
  free(password);
  if(has_active_task()) return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  if(config_error) return send_json_error_detail(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "could not read storage configuration", "storage_config_read_failed", strerror(config_error));
  saved_smb_t added = {.address = a, .authenticated = 1, .checked_at = time(NULL)};
  if(local) {
    DIR *dir = opendir(a.path);
    if(!dir) {
      int error = errno;
      return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST, strerror(error), path_error_code(error), strerror(error));
    }
    closedir(dir);
    local_space(&added);
  } else if(net_path(a.path)) {
    struct statvfs space;
    if(net_probe(&a,&space)) {
      int error=errno;
      return send_json_error_detail(conn,MHD_HTTP_BAD_GATEWAY,"Remote connection failed",
                                    net_error_code(error),net_error_detail());
    }
    added.available=space.f_frsize?1:-1;
    if(space.f_frsize) {
      added.free_bytes=(unsigned long long)space.f_bavail*space.f_frsize;
      added.total_bytes=(unsigned long long)space.f_blocks*space.f_frsize;
    }
  } else {
    smb_lock();
    struct smb2_context *ctx = connect_address(&a);
    struct smb2dir *dir = ctx ? smb2_opendir(ctx, a.relative) : NULL;
    if(!dir) {
      int password_required = !ctx && errno == EACCES;
      int error = ctx ? nterror_to_errno(smb2_get_nterror(ctx)) : errno;
      char detail[512];
      snprintf(detail, sizeof(detail), "%s", ctx ? smb2_get_error(ctx) : client_error);
      const char *code = password_required ? "smb_password_required" :
        smb_error_code(error, ctx ? smb2_get_nterror(ctx) : client_status, detail, "smb_connect_failed");
      if(ctx) SMB_DEBUG("opendir failed server=%s share=%s path=%s ntstatus=0x%08x detail=%s",
                        a.server, a.share, a.relative, (unsigned int)smb2_get_nterror(ctx), detail);
      if(ctx) smb2_destroy_context(ctx);
      smb_unlock();
      return send_json_error_detail(conn, MHD_HTTP_BAD_GATEWAY, "SMB connection failed",
                                    code, detail);
    }
    smb2_closedir(ctx, dir);
    struct smb2_statvfs vfs;
    int has_space = !smb2_statvfs(ctx, a.relative, &vfs);
    if(!has_space) SMB_DEBUG("capacity query failed server=%s share=%s path=%s ntstatus=0x%08x detail=%s",
                            a.server, a.share, a.relative, (unsigned int)smb2_get_nterror(ctx), smb2_get_error(ctx));
    smb2_destroy_context(ctx);
    smb_unlock();
    added.available = has_space ? 1 : -1;
    if(has_space) {
      unsigned long long block = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
      added.free_bytes = vfs.f_bavail * block;
      added.total_bytes = vfs.f_blocks * block;
    }
  }
  pthread_mutex_lock(&config_mutex);
  size_t index = connection_index(a.path);
  int duplicate = !connecting && index < connection_count;
  if(connecting && (index == connection_count ||
      strcmp(connections[index].address.user, a.user) || strcmp(connections[index].address.domain, a.domain))) {
    pthread_mutex_unlock(&config_mutex);
    return send_json_error_detail(conn, MHD_HTTP_CONFLICT, "Remote connection changed; try again", "smb_connection_changed", NULL);
  }
  if(index == connection_count && connection_count == SMB_CONNECTION_MAX) {
    pthread_mutex_unlock(&config_mutex);
    return send_json_error_detail(conn, MHD_HTTP_BAD_REQUEST, "too many saved paths", "storage_limit_reached", NULL);
  }
  saved_smb_t old = connections[index];
  size_t old_count = connection_count;
  connections[index] = added;
  if(index == connection_count) connection_count++;
  int result = connecting ? 0 : save_config(config_kind(a.path));
  if(result) { connections[index] = old; connection_count = old_count; }
  pthread_mutex_unlock(&config_mutex);
  if(result) return send_json_error_detail(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "could not save storage configuration", "storage_config_save_failed", strerror(errno));
  strbuf_t json = {0};
  strbuf_append(&json, "{\"ok\":true,\"path\":");
  json_escape(&json, a.path);
  strbuf_append(&json, ",\"user\":"); json_escape(&json, a.user);
  strbuf_append(&json, ",\"domain\":"); json_escape(&json, a.domain);
  strbuf_append(&json, duplicate ? ",\"duplicate\":true}" : "}");
  return send_buffer(conn, MHD_HTTP_OK, json.data, "application/json");
}

enum MHD_Result api_smb_remove(struct MHD_Connection *conn, const char *body, size_t size) {
  char *path = fs_path_value(body_form_value(body, size, "path"));
  if(!path || (!remote_path(path) && path[0] != '/') || strlen(path) >= PATH_MAX) {
    free(path);
    return send_json_error(conn, MHD_HTTP_BAD_REQUEST, "invalid saved path");
  }
  if(path[0] == '/') trim_local_path(path);
  if(has_active_task()) {
    free(path);
    return send_json_error(conn, MHD_HTTP_CONFLICT, "another task is running");
  }
  pthread_once(&config_once, load_config);
  if(config_error) {
    free(path);
    return send_json_error_detail(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "could not read storage configuration", "storage_config_read_failed", strerror(config_error));
  }
  pthread_mutex_lock(&config_mutex);
  size_t index = connection_index(path);
  free(path);
  int result = 0;
  if(index < connection_count) {
    saved_smb_t removed = connections[index];
    size_t following = connection_count - index - 1;
    memmove(connections + index, connections + index + 1, following * sizeof(*connections));
    connection_count--;
    result = save_config(config_kind(removed.address.path));
    if(result) {
      memmove(connections + index + 1, connections + index, following * sizeof(*connections));
      connections[index] = removed;
      connection_count++;
    } else memset(connections + connection_count, 0, sizeof(*connections));
  }
  pthread_mutex_unlock(&config_mutex);
  if(result) return send_json_error_detail(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "could not save storage configuration", "storage_config_save_failed", strerror(errno));
  return send_buffer(conn, MHD_HTTP_OK, strdup("{\"ok\":true}"), "application/json");
}

static void *refresh_spaces(void *current) {
  pthread_mutex_lock(&config_mutex);
  size_t count = connection_count;
  saved_smb_t *snapshot = malloc(count * sizeof(*snapshot));
  if(snapshot) memcpy(snapshot, connections, count * sizeof(*snapshot));
  pthread_mutex_unlock(&config_mutex);
  for(size_t i = 0; snapshot && i < count; i++) {
    saved_smb_t *saved = &snapshot[i];
    if(remote_path(saved->address.path) && (strcmp(saved->address.path, current) ||
       (!saved->authenticated && saved->address.user[0] && strcasecmp(saved->address.user, "Guest") &&
        strcasecmp(saved->address.user, "anonymous")))) continue;
    if(time(NULL) - saved->checked_at >= 30 && !has_active_task()) {
      if(saved->address.path[0] == '/') {
        local_space(saved);
      } else if(net_path(saved->address.path)) {
        struct statvfs vfs;
        saved->available=net_probe(&saved->address,&vfs)?0:vfs.f_frsize?1:-1;
        if(saved->available==1) {
          saved->free_bytes=(unsigned long long)vfs.f_bavail*vfs.f_frsize;
          saved->total_bytes=(unsigned long long)vfs.f_blocks*vfs.f_frsize;
        }
      } else {
        smb_lock();
        struct smb2_context *ctx = connect_address(&saved->address);
        struct smb2_statvfs vfs;
        saved->available = ctx ? (!smb2_statvfs(ctx, saved->address.relative, &vfs) ? 1 : -1) : 0;
        if(saved->available == 1) {
          unsigned long long block = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
          saved->free_bytes = vfs.f_bavail * block;
          saved->total_bytes = vfs.f_blocks * block;
        }
        if(ctx) smb2_destroy_context(ctx);
        smb_unlock();
      }
      saved->checked_at = time(NULL);
      pthread_mutex_lock(&config_mutex);
      if(i < connection_count && !strcmp(saved->address.path, connections[i].address.path) &&
         !strcmp(saved->address.user, connections[i].address.user) &&
         !strcmp(saved->address.domain, connections[i].address.domain) &&
         !strcmp(saved->address.password, connections[i].address.password)) {
        connections[i].available = saved->available;
        connections[i].checked_at = saved->checked_at;
        connections[i].free_bytes = saved->free_bytes;
        connections[i].total_bytes = saved->total_bytes;
      }
      pthread_mutex_unlock(&config_mutex);
    }
  }
  free(snapshot);
  free(current);
  pthread_mutex_lock(&config_mutex);
  refreshing_spaces = 0;
  pthread_mutex_unlock(&config_mutex);
  return NULL;
}

void smb_append_spaces(strbuf_t *json, const char *current, int *first) {
  pthread_once(&config_once, load_config);
  pthread_mutex_lock(&config_mutex);
  size_t current_index = connection_count, current_length = 0;
  int stale = 0;
  for(size_t i = 0; i < connection_count; i++) {
    size_t len = strlen(connections[i].address.path);
    if(len > current_length && smb_path_current(current, connections[i].address.path)) {
      current_length = len;
      current_index = i;
    }
    if(connections[i].address.path[0] == '/' && time(NULL) - connections[i].checked_at >= 30) stale = 1;
  }
  if(current_index < connection_count && time(NULL) - connections[current_index].checked_at >= 30) stale = 1;
  /* Offline servers must not delay returning the shared disk list. */
  if(stale && !refreshing_spaces) {
    pthread_t thread;
    char *path = strdup(current_index < connection_count ? connections[current_index].address.path : "");
    if(path) {
      refreshing_spaces = 1;
      if(pthread_create(&thread, NULL, refresh_spaces, path)) { refreshing_spaces = 0; free(path); }
      else pthread_detach(thread);
    }
  }
  for(int local = 0; local <= 1; local++) {
    for(size_t i = 0; i < connection_count; i++) {
      saved_smb_t *saved = &connections[i];
      if((saved->address.path[0] == '/') != local) continue;
      if(!*first) strbuf_append(json, ",");
      *first = 0;
      strbuf_append(json, "{\"custom\":true,\"label\":");
      json_escape(json, saved->address.path);
      strbuf_append(json, ",\"path\":");
      json_escape(json, saved->address.path);
      strbuf_append(json, ",\"user\":"); json_escape(json, saved->address.user);
      strbuf_append(json, ",\"domain\":"); json_escape(json, saved->address.domain);
      strbuf_printf(json, ",\"free\":%llu,\"total\":%llu,\"available\":%s,\"current\":%s}",
        saved->free_bytes, saved->total_bytes, saved->available < 0 ? "null" : saved->available ? "true" : "false",
        i == current_index ? "true" : "false");
    }
  }
  pthread_mutex_unlock(&config_mutex);
}
