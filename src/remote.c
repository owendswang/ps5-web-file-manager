#define WFM_VFS_IMPLEMENTATION
#include "remote.h"
#include "transfer.h"
#include "path_util.h"
#include <curl/curl.h>
#include <yxml.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NET_FD_BASE 0x50000000
#define NET_FILES 64
#define NET_LIST_LIMIT (16 * 1024 * 1024)
typedef struct net_entry { char name[256]; struct stat st; } net_entry_t;
struct net_dir {
  char path[PATH_MAX];
  net_entry_t *items;
  size_t count, capacity, position;
  struct dirent entry;
  struct stat self;
  int found;
  struct net_dir *next;
};
typedef struct net_file {
  smb_address_t auth;
  char path[PATH_MAX];
  CURL *curl;
  struct stat st;
  off_t position, cache_offset;
  unsigned char *cache, *ring;
  size_t cache_size, cache_capacity, capacity, head, used;
  curl_off_t expected;
  int writing, upload_needed, started, done, finished, joined, error;
  pthread_t thread;
  pthread_mutex_t lock;
  pthread_cond_t ready, space;
} net_file_t;
static net_file_t *files[NET_FILES];
static net_dir_t *directories;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
static int curl_init_error;
static struct curl_slist ftp_utf8 = {(char *)"*OPTS UTF8 ON",NULL};
static _Thread_local char last_error[CURL_ERROR_SIZE];
static _Thread_local CURLcode last_curl_error;

static void init_curl(void) { curl_init_error = curl_global_init(CURL_GLOBAL_DEFAULT); }
int net_path(const char *p) {
  return p && (!strncmp(p,"ftp://",6) || !strncmp(p,"ftps://",7) ||
    !strncmp(p,"webdav://",9) || !strncmp(p,"webdavs://",10) ||
    !strncmp(p,"dav://",6) || !strncmp(p,"davs://",7) ||
    !strncmp(p,"http://",7) || !strncmp(p,"https://",8));
}
int remote_path(const char *p) { return smb_path(p) || net_path(p); }
static int ftp_path(const char *p) { return !strncmp(p,"ftp://",6) || !strncmp(p,"ftps://",7); }
static int transport_url(const char *path, char *out, size_t size) {
  const char *rest = strstr(path, "://");
  if(!rest) { errno = EINVAL; return -1; }
  const char *scheme = ftp_path(path) ? (!strncmp(path,"ftps:",5) ? "ftps" : "ftp") :
    (!strncmp(path,"webdavs:",8) || !strncmp(path,"davs:",5) || !strncmp(path,"https:",6) ? "https" : "http");
  if(snprintf(out,size,"%s%s",scheme,rest) >= (int)size) { errno = ENAMETOOLONG; return -1; }
  return 0;
}
static int copy_text(char *out, size_t size, const char *text) {
  if(strlen(text) >= size) { errno = ENAMETOOLONG; return -1; }
  strcpy(out,text); return 0;
}
static int clean_text(const char *text) {
  for(const unsigned char *p=(const unsigned char *)text; *p; p++)
    if(*p<32 || *p==127)return 0;
  return 1;
}
static int clean_component(const char *text) { return clean_text(text) && !strchr(text,'\\'); }
static char *encode_path(const char *raw) {
  char *encoded=curl_easy_escape(NULL,raw,0);
  if(encoded) {
    char *source=encoded,*target=encoded;
    while(*source) {
      if(!strncmp(source,"%2F",3)) { *target++='/'; source+=3; }
      else *target++=*source++;
    }
    *target=0;
  }
  return encoded;
}
int net_join(char *out,size_t size,const char *dir,const char *name) {
  char *encoded=curl_easy_escape(NULL,name,0);
  if(!encoded) { errno=ENOMEM; return -1; }
  int length=snprintf(out,size,"%s%s%s",dir,dir[strlen(dir)-1]=='/'?"":"/",encoded);
  curl_free(encoded);
  if(length<0 || (size_t)length>=size) { errno=ENAMETOOLONG; return -1; }
  return 0;
}
int net_parse_address(const char *input, smb_address_t *a) {
  char transport[PATH_MAX], *user=NULL, *password=NULL, *host=NULL, *port=NULL;
  CURLU *url=NULL;
  int result=-1;
  memset(a,0,sizeof(*a));
  pthread_once(&curl_once,init_curl);
  if(curl_init_error || !net_path(input) || !clean_component(input) ||
     transport_url(input,transport,sizeof(transport))) { errno=EINVAL; return -1; }
  const char *authority=strstr(transport,"://")+3, *path=strchr(authority,'/');
  size_t base=path?(size_t)(path-transport):strlen(transport);
  if(base>=sizeof(transport)) { errno=ENAMETOOLONG; return -1; }
  transport[base]=0;
  url=curl_url();
  if(!url || curl_url_set(url,CURLUPART_URL,transport,0)) { errno=EINVAL; goto done; }
  if(curl_url_get(url,CURLUPART_HOST,&host,0)) { errno=EINVAL; goto done; }
  CURLUcode user_result=curl_url_get(url,CURLUPART_USER,&user,CURLU_URLDECODE);
  CURLUcode password_result=curl_url_get(url,CURLUPART_PASSWORD,&password,CURLU_URLDECODE);
  if((user_result && user_result!=CURLUE_NO_USER) || (password_result && password_result!=CURLUE_NO_PASSWORD)) { errno=EINVAL; goto done; }
  if((user && copy_text(a->user,sizeof(a->user),user)) ||
     (password && copy_text(a->password,sizeof(a->password),password)))goto done;
  if(!clean_text(a->user) || !clean_text(a->password)) { errno=EINVAL; goto done; }
  for(char *p=host;*p;p++)*p=(char)tolower((unsigned char)*p);
  (void)curl_url_get(url,CURLUPART_PORT,&port,0);
  if(snprintf(a->server,sizeof(a->server),"%s%s%s",host,port?":":"",port?port:"") >= (int)sizeof(a->server)) { errno=ENAMETOOLONG; goto done; }
  const char *raw=strchr(strstr(input,"://")+3,'/');
  int length=0;
  char *decoded=curl_easy_unescape(NULL,raw?raw:"/",0,&length);
  if(!decoded) { errno=ENOMEM; goto done; }
  if((size_t)length!=strlen(decoded) || !clean_component(decoded) || strlen(decoded)>=sizeof(a->relative)) {
    curl_free(decoded); errno=EINVAL; goto done;
  }
  int absolute_ftp=ftp_path(input) && decoded[0]=='/' && decoded[1]=='/';
  char normalized[PATH_MAX]; size_t used=0;
  for(char *part=decoded;*part;) {
    while(*part=='/')part++;
    if(!*part)break;
    char *end=strchr(part,'/'); size_t n=end?(size_t)(end-part):strlen(part);
    if((n==1 && part[0]=='.') || (n==2 && !memcmp(part,"..",2))) {
      curl_free(decoded); errno=EINVAL; goto done;
    }
    if(used+n+1>=sizeof(normalized)) { curl_free(decoded); errno=ENAMETOOLONG; goto done; }
    normalized[used++]='/'; memcpy(normalized+used,part,n); used+=n;
    part+=n;
  }
  normalized[used]=0;
  curl_free(decoded);
  const char *scheme=ftp_path(input)?(!strncmp(input,"ftps:",5)?"ftps":"ftp"):
    (!strncmp(input,"webdavs:",8)||!strncmp(input,"davs:",5)||!strncmp(input,"https:",6)?"webdavs":"webdav");
  char *encoded=encode_path(normalized[0]?normalized:"/");
  if(!encoded) { errno=ENOMEM; goto done; }
  int too_long=snprintf(a->path,sizeof(a->path),"%s://%s%s%s",scheme,a->server,
      absolute_ftp?"/%2F":"",absolute_ftp?encoded+1:used?encoded:"")>= (int)sizeof(a->path) ||
     copy_text(a->relative,sizeof(a->relative),encoded);
  curl_free(encoded);
  if(too_long) { errno=ENAMETOOLONG; goto done; }
  result=0;
done:
  curl_free(user); curl_free(password); curl_free(host); curl_free(port); curl_url_cleanup(url);
  return result;
}

const char *net_error_detail(void) { return last_error; }
const char *net_error_code(int error) {
  if(last_curl_error==CURLE_PEER_FAILED_VERIFICATION || last_curl_error==CURLE_SSL_CACERT_BADFILE)return "remote_certificate_error";
  if(error==EACCES)return "remote_password_required";
  if(error==ENOENT)return "storage_path_not_found";
  if(error==ENOTDIR)return "storage_path_not_directory";
  if(error==ETIMEDOUT)return "remote_timeout";
  if(error==ENOSPC)return "no_space";
  if(error==ENOTSUP || error==ESPIPE)return "remote_unsupported";
  return "remote_connect_failed";
}
static int perform(CURL *curl, int metadata) {
  last_error[0]=0;
  curl_easy_setopt(curl,CURLOPT_ERRORBUFFER,last_error);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT,metadata?20L:0L);
  CURLcode code=curl_easy_perform(curl);
  curl_easy_setopt(curl,CURLOPT_ERRORBUFFER,NULL);
  long status=0; curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
  last_curl_error=code;
  char *scheme=NULL; curl_easy_getinfo(curl,CURLINFO_SCHEME,&scheme);
  /* libcurl validates FTP replies, including server-specific ABOR responses. */
  if(code==CURLE_OK && ((status>=200 && status<300) || (scheme && (!strcmp(scheme,"ftp") || !strcmp(scheme,"ftps")))))return 0;
  int error=EIO;
  if(status==401 || status==407 || code==CURLE_LOGIN_DENIED)error=EACCES;
  else if(status==403 || status==550 || code==CURLE_REMOTE_ACCESS_DENIED)error=EACCES;
  else if(status==404 || status==410 || code==CURLE_REMOTE_FILE_NOT_FOUND || code==CURLE_FTP_COULDNT_RETR_FILE)error=ENOENT;
  else if((status>=300 && status<400) || status==405 || status==501 || code==CURLE_RANGE_ERROR || code==CURLE_BAD_DOWNLOAD_RESUME || code==CURLE_FTP_COULDNT_USE_REST)error=ENOTSUP;
  else if(status==409)error=ENOENT;
  else if(status==412)error=EEXIST;
  else if(status==507 || code==CURLE_REMOTE_DISK_FULL)error=ENOSPC;
  else if(code==CURLE_OPERATION_TIMEDOUT)error=ETIMEDOUT;
  else if(code==CURLE_COULDNT_CONNECT)error=ECONNREFUSED;
  else if(code==CURLE_COULDNT_RESOLVE_HOST)error=EHOSTUNREACH;
  else if(code==CURLE_OUT_OF_MEMORY)error=ENOMEM;
  if(!last_error[0])snprintf(last_error,sizeof(last_error),"Remote response %ld: %s",status,curl_easy_strerror(code));
#if WFM_DEBUG
  fprintf(stderr,"Remote transfer: status=%ld curl=%d error=%d %s\n",status,(int)code,error,last_error);
#endif
  errno=error; return -1;
}
static size_t discard_body(char *data,size_t size,size_t count,void *unused) {
  (void)data; (void)unused; return size*count;
}
static int tune_socket(void *unused,curl_socket_t fd,curlsocktype purpose) {
  (void)unused; (void)purpose;
  transfer_socket_option(fd,SOL_SOCKET,SO_SNDBUF,TRANSFER_SOCKET_BUFFER_SIZE,"Remote","SO_SNDBUF");
  transfer_socket_option(fd,SOL_SOCKET,SO_RCVBUF,TRANSFER_SOCKET_BUFFER_SIZE,"Remote","SO_RCVBUF");
  return CURL_SOCKOPT_OK;
}
static CURL *request_new(const char *path,const smb_address_t *auth,int directory) {
  char url[PATH_MAX], authority[512];
  const char *rest=strstr(path,"://"), *slash=rest?strchr(rest+3,'/'):NULL;
  if(!rest || transport_url(path,url,sizeof(url)))return NULL;
  const char *transport_rest=strstr(url,"://"), *transport_slash=strchr(transport_rest+3,'/');
  size_t length=transport_slash?(size_t)(transport_slash-url):strlen(url);
  if(length>=sizeof(authority)) { errno=ENAMETOOLONG; return NULL; }
  memcpy(authority,url,length); authority[length]=0;
  CURLU *parsed=curl_url();
  if(!parsed) { errno=ENOMEM; return NULL; }
  char raw[PATH_MAX];
  if(snprintf(raw,sizeof(raw),"%s%s",slash?slash:"/",directory && slash && slash[strlen(slash)-1]!='/'?"/":"") >= (int)sizeof(raw) ||
     curl_url_set(parsed,CURLUPART_URL,authority,0) ||
     curl_url_set(parsed,CURLUPART_PATH,raw,0)) { curl_url_cleanup(parsed); errno=EINVAL; return NULL; }
  char *encoded=NULL;
  if(curl_url_get(parsed,CURLUPART_URL,&encoded,0)) { curl_url_cleanup(parsed); errno=EINVAL; return NULL; }
  CURL *curl=curl_easy_init();
  if(curl) {
    curl_easy_setopt(curl,CURLOPT_URL,encoded);
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_PROXY,"");
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,8L);
    curl_easy_setopt(curl,CURLOPT_LOW_SPEED_LIMIT,1L);
    curl_easy_setopt(curl,CURLOPT_LOW_SPEED_TIME,30L);
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(curl,CURLOPT_PROTOCOLS_STR,"ftp,ftps,http,https");
    curl_easy_setopt(curl,CURLOPT_FTP_SKIP_PASV_IP,1L);
    if(ftp_path(path))curl_easy_setopt(curl,CURLOPT_QUOTE,&ftp_utf8);
    curl_easy_setopt(curl,CURLOPT_SOCKOPTFUNCTION,tune_socket);
    if(ftp_path(path) || auth->user[0] || auth->password[0]) {
      curl_easy_setopt(curl,CURLOPT_USERNAME,auth->user[0]?auth->user:(ftp_path(path)?"anonymous":""));
      curl_easy_setopt(curl,CURLOPT_PASSWORD,auth->password);
    }
    if(!ftp_path(path) && auth->user[0])curl_easy_setopt(curl,CURLOPT_HTTPAUTH,(long)(CURLAUTH_BASIC|CURLAUTH_DIGEST|CURLAUTH_NTLM));
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,discard_body);
    curl_easy_setopt(curl,CURLOPT_BUFFERSIZE,65536L);
    const char *dir=getenv("WFM_CONFIG_DIR"); char ca[PATH_MAX];
    snprintf(ca,sizeof(ca),"%s/ca-bundle.crt",dir && dir[0]?dir:"/data/wfm/config");
    /* PS5 payloads can have a non-root real UID and a privileged effective UID. */
    struct stat ca_st;
    if(!stat(ca,&ca_st) && S_ISREG(ca_st.st_mode))curl_easy_setopt(curl,CURLOPT_CAINFO,ca);
  }
  curl_free(encoded); curl_url_cleanup(parsed);
  return curl;
}
static dev_t device_id(const char *path) {
  const char *end=strchr(strstr(path,"://")+3,'/');
  uint32_t hash=2166136261u;
  for(const char *p=path;*p && p!=end;p++)hash=(hash^(unsigned char)*p)*16777619u;
  return (dev_t)(0x90000000u|(hash&0x0fffffffu));
}
static void directory_stat(struct stat *st,const char *path) {
  /* Local UIDs/mode bits cannot represent the authenticated remote account. */
  memset(st,0,sizeof(*st)); st->st_mode=S_IFDIR|0777; st->st_nlink=1; st->st_dev=device_id(path);
}
static int add_entry(net_dir_t *dir,const char *name,const struct stat *st) {
  if(!name[0] || !strcmp(name,".") || !strcmp(name,".."))return 0;
  if(strchr(name,'/') || !clean_component(name)) { errno=EIO; return -1; }
  if(strlen(name)>=sizeof(dir->items[0].name)) { errno=ENAMETOOLONG; return -1; }
  if(dir->count==32768) { errno=EFBIG; return -1; }
  if(dir->count==dir->capacity) {
    size_t capacity=dir->capacity?dir->capacity*2:32;
    net_entry_t *items=realloc(dir->items,capacity*sizeof(*items));
    if(!items)return -1;
    dir->items=items; dir->capacity=capacity;
  }
  net_entry_t *entry=&dir->items[dir->count++];
  strcpy(entry->name,name); entry->st=*st;
  return 0;
}
typedef struct memory_body { char *data; size_t size; int error; } memory_body_t;
static size_t collect_body(char *data,size_t size,size_t count,void *opaque) {
  memory_body_t *body=opaque;
  if(size && count>SIZE_MAX/size)return 0;
  size*=count;
  if(size>NET_LIST_LIMIT-body->size) { body->error=EFBIG; return 0; }
  char *next=realloc(body->data,body->size+size+1);
  if(!next) { body->error=ENOMEM; return 0; }
  body->data=next; memcpy(next+body->size,data,size); body->size+=size; next[body->size]=0;
  return size;
}
static const char *local_name(const char *name) { const char *p=strrchr(name,':'); return p?p+1:name; }
static int response_status(const char *text) { const char *p=strchr(text,' '); return p?atoi(p+1):0; }
/* Only immediate children and successful DAV propstats become filesystem entries. */
static int parse_dav(net_dir_t *dir,const char *data,size_t size,int depth,struct statvfs *space) {
  yxml_t xml; char stack[8192], text[PATH_MAX], href[PATH_MAX];
  struct stat prop={0}, resource={0};
  int level=0, field=0, field_level=0, prop_level=0, response_level=0, status=0, collection=0;
  unsigned long long used=0,available=0,prop_used=0,prop_available=0;
  int has_used=0,has_available=0,prop_has_used=0,prop_has_available=0,valid=0;
  size_t written=0;
  yxml_init(&xml,stack,sizeof(stack)); href[0]=0;
  for(size_t i=0;i<size;i++) {
    yxml_ret_t token=yxml_parse(&xml,(unsigned char)data[i]);
    if(token<0) { errno=EPROTO; return -1; }
    const char *name=local_name(xml.elem);
    if(token==YXML_ELEMSTART) {
      level++;
      if(!strcmp(name,"response")) { memset(&resource,0,sizeof(resource)); href[0]=0; valid=0; response_level=level; }
      if(!strcmp(name,"propstat")) {
        memset(&prop,0,sizeof(prop)); prop_level=level; status=0; collection=0;
        prop_has_used=prop_has_available=0;
      }
      if(prop_level && !strcmp(name,"collection"))collection=1;
      int which=!strcmp(name,"href")?1:!strcmp(name,"getcontentlength")?2:
        !strcmp(name,"getlastmodified")?3:!strcmp(name,"status")?4:
        !strcmp(name,"quota-used-bytes")?5:!strcmp(name,"quota-available-bytes")?6:0;
      if(which) { field=which; field_level=level; written=0; text[0]=0; }
    } else if(token==YXML_CONTENT && field) {
      size_t n=strlen(xml.data);
      if(n>=sizeof(text)-written) { errno=ENAMETOOLONG; return -1; }
      memcpy(text+written,xml.data,n); written+=n; text[written]=0;
    } else if(token==YXML_ELEMEND) {
      if(field && level==field_level) {
        if(field==1)strcpy(href,text);
        if(field==2) {
          char *end; errno=0; unsigned long long n=strtoull(text,&end,10);
          if(errno || *end || n>INT64_MAX) { errno=EIO; return -1; }
          prop.st_size=(off_t)n;
        }
        if(field==3) { time_t when=curl_getdate(text,NULL); if(when>=0)prop.st_mtime=when; }
        if(field==4)status=response_status(text);
        if((field==5 || field==6) && prop_level && text[0] &&
           strspn(text,"0123456789")==strlen(text)) {
          errno=0; unsigned long long value=strtoull(text,NULL,10);
          if(!errno && field==5) { prop_used=value; prop_has_used=1; }
          if(!errno && field==6) { prop_available=value; prop_has_available=1; }
        }
        field=0;
      }
      if(level==prop_level) {
        if(status>=200 && status<300) {
          valid=1;
          if(collection)resource.st_mode=S_IFDIR|0777;
          if(prop.st_size)resource.st_size=prop.st_size;
          if(prop.st_mtime)resource.st_mtime=prop.st_mtime;
          if(prop_has_used) { used=prop_used; has_used=1; }
          if(prop_has_available) { available=prop_available; has_available=1; }
        }
        prop_level=0;
      }
      if(level==response_level && href[0] && valid) {
        const char *encoded=href;
        if(strstr(href,"://")) { encoded=strchr(strstr(href,"://")+3,'/'); if(!encoded)encoded="/"; }
        int n=0; char *path=curl_easy_unescape(NULL,encoded,0,&n);
        if(!path || (size_t)n!=strlen(path) || !clean_component(path)) { curl_free(path); errno=EPROTO; return -1; }
        while(n>1 && path[n-1]=='/')path[--n]=0;
        char *canonical=encode_path(path);
        if(!canonical) { curl_free(path); errno=ENOMEM; return -1; }
        const char *root=strchr(strstr(dir->path,"://")+3,'/'); if(!root)root="/";
        resource.st_mode=resource.st_mode?resource.st_mode:S_IFREG|0777;
        resource.st_nlink=1; resource.st_dev=device_id(dir->path);
        if(!strcmp(root,canonical)) { dir->self=resource; dir->found=1; }
        else if(depth && (!strcmp(root,"/") || (!strncmp(root,canonical,strlen(root)) && canonical[strlen(root)]=='/'))) {
          const char *child=canonical+(!strcmp(root,"/")?1:strlen(root)+1);
          if(!strchr(child,'/')) {
            char *decoded_child=curl_easy_unescape(NULL,child,0,NULL);
            int error=!decoded_child || add_entry(dir,decoded_child,&resource);
            curl_free(decoded_child);
            if(error) { curl_free(path); curl_free(canonical); return -1; }
          }
        }
        curl_free(path); curl_free(canonical);
      }
      level--;
    }
  }
  if(yxml_eof(&xml)<0 || !dir->found) { errno=EPROTO; return -1; }
  if(space && has_used && has_available && used<=ULLONG_MAX-available) {
    memset(space,0,sizeof(*space)); space->f_bsize=space->f_frsize=1;
    space->f_blocks=used+available; space->f_bavail=space->f_bfree=available;
  }
  return 0;
}
static int dav_list(net_dir_t *dir,const smb_address_t *auth,int depth,struct statvfs *space) {
  static const char body[]="<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/><d:getcontentlength/><d:getlastmodified/><d:quota-used-bytes/><d:quota-available-bytes/></d:prop></d:propfind>";
  CURL *curl=request_new(dir->path,auth,depth); if(!curl)return -1;
  struct curl_slist *headers=NULL;
  headers=curl_slist_append(headers,depth?"Depth: 1":"Depth: 0");
  headers=curl_slist_append(headers,"Content-Type: application/xml; charset=utf-8");
  memory_body_t response={0};
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS,body);
  curl_easy_setopt(curl,CURLOPT_CUSTOMREQUEST,"PROPFIND");
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,collect_body);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,&response);
  int result=perform(curl,1);
  long status=0; curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
  if(result && status>=300 && status<400) {
    /* Some DAV servers require a collection's trailing slash, including Depth 0. */
    char *url=NULL; curl_easy_getinfo(curl,CURLINFO_EFFECTIVE_URL,&url);
    char directory_url[PATH_MAX+1];
    if(url && url[strlen(url)-1]!='/' &&
       snprintf(directory_url,sizeof(directory_url),"%s/",url)<(int)sizeof(directory_url)) {
      free(response.data); memset(&response,0,sizeof(response));
      curl_easy_setopt(curl,CURLOPT_URL,directory_url);
      result=perform(curl,1);
    }
  }
  if(response.error) { errno=response.error; result=-1; }
  if(!result)result=parse_dav(dir,response.data,response.size,depth,space);
  int error=errno;
  free(response.data); curl_slist_free_all(headers); curl_easy_cleanup(curl);
  errno=error; return result;
}
static long ftp_entry(const void *info,void *opaque,int remains) {
  (void)remains; const struct curl_fileinfo *file=info; net_dir_t *dir=opaque;
  struct stat st={0};
  if(file->filetype!=CURLFILETYPE_FILE && file->filetype!=CURLFILETYPE_DIRECTORY && file->filetype!=CURLFILETYPE_SYMLINK)return CURL_CHUNK_BGN_FUNC_SKIP;
  st.st_mode=(file->filetype==CURLFILETYPE_DIRECTORY?S_IFDIR:file->filetype==CURLFILETYPE_SYMLINK?S_IFLNK:S_IFREG)|0777;
  st.st_size=file->size>=0?(off_t)file->size:0; st.st_nlink=1; st.st_dev=device_id(dir->path);
  if(file->strings.time) {
    time_t value=curl_getdate(file->strings.time,NULL);
    if(value<0 && strchr(file->strings.time,':') && !strchr(file->strings.time,'-')) {
      time_t now=time(NULL); struct tm today; gmtime_r(&now,&today);
      char date[96]; snprintf(date,sizeof(date),"%s %d",file->strings.time,today.tm_year+1900);
      value=curl_getdate(date,NULL);
      if(value>now+86400) {
        snprintf(date,sizeof(date),"%s %d",file->strings.time,today.tm_year+1899);
        value=curl_getdate(date,NULL);
      }
    }
    if(value>=0)st.st_mtime=value;
  }
  return add_entry(dir,file->filename,&st)?CURL_CHUNK_BGN_FUNC_FAIL:CURL_CHUNK_BGN_FUNC_SKIP;
}
static size_t ftp_reply(char *data,size_t size,size_t count,void *opaque) {
  size*=count;
  if(size>=4 && isdigit((unsigned char)data[0]) && isdigit((unsigned char)data[1]) &&
     isdigit((unsigned char)data[2]) && data[3]==' ')
    *(long *)opaque=(data[0]-'0')*100+(data[1]-'0')*10+data[2]-'0';
  return size;
}
static int ftp_list(net_dir_t *dir,const smb_address_t *auth) {
  CURL *curl=request_new(dir->path,auth,1); if(!curl)return -1;
  char *url=NULL; curl_easy_getinfo(curl,CURLINFO_EFFECTIVE_URL,&url);
  char wildcard[PATH_MAX*3];
  if(!url || snprintf(wildcard,sizeof(wildcard),"%s*",url)>=(int)sizeof(wildcard)) { curl_easy_cleanup(curl); errno=ENAMETOOLONG; return -1; }
  curl_easy_setopt(curl,CURLOPT_URL,wildcard);
  curl_easy_setopt(curl,CURLOPT_WILDCARDMATCH,1L);
  curl_easy_setopt(curl,CURLOPT_CHUNK_BGN_FUNCTION,ftp_entry);
  curl_easy_setopt(curl,CURLOPT_CHUNK_DATA,dir);
  curl_easy_setopt(curl,CURLOPT_CUSTOMREQUEST,"LIST -a");
  long code=0;
  curl_easy_setopt(curl,CURLOPT_HEADERFUNCTION,ftp_reply);
  curl_easy_setopt(curl,CURLOPT_HEADERDATA,&code);
  int result=perform(curl,1);
  if(result && code>=500 && code<=504) {
    curl_easy_setopt(curl,CURLOPT_CUSTOMREQUEST,"LIST");
    dir->count=0; result=perform(curl,1);
  }
  if(result && !dir->count && last_curl_error==CURLE_REMOTE_FILE_NOT_FOUND && (code==226 || code==250))result=0;
  if(!result) { directory_stat(&dir->self,dir->path); dir->found=1; }
  int error=errno; curl_easy_cleanup(curl); errno=error; return result;
}
static int fetch_directory(net_dir_t *dir,const smb_address_t *auth,int depth,struct statvfs *space) {
  return ftp_path(dir->path)?ftp_list(dir,auth):dav_list(dir,auth,depth,space);
}
int net_probe(const smb_address_t *auth,struct statvfs *space) {
  net_dir_t dir={0}; strcpy(dir.path,auth->path);
  memset(space,0,sizeof(*space));
  int result=fetch_directory(&dir,auth,0,space);
  if(!result && !S_ISDIR(dir.self.st_mode)) { errno=ENOTDIR; result=-1; }
  int error=errno; free(dir.items); errno=error; return result;
}
net_dir_t *net_opendir(const char *path) {
  smb_address_t auth;
  if(storage_remote_address(path,&auth))return NULL;
  net_dir_t *dir=calloc(1,sizeof(*dir)); if(!dir)return NULL;
  if(copy_text(dir->path,sizeof(dir->path),path) || fetch_directory(dir,&auth,1,NULL)) {
    int error=errno; free(dir->items); free(dir); errno=error; return NULL;
  }
  if(!S_ISDIR(dir->self.st_mode)) { free(dir->items); free(dir); errno=ENOTDIR; return NULL; }
  pthread_mutex_lock(&registry_lock); dir->next=directories; directories=dir; pthread_mutex_unlock(&registry_lock);
  return dir;
}
struct dirent *net_readdir(net_dir_t *dir) {
  if(dir->position==dir->count)return NULL;
  net_entry_t *entry=&dir->items[dir->position++];
  memset(&dir->entry,0,sizeof(dir->entry)); strcpy(dir->entry.d_name,entry->name);
  dir->entry.d_type=S_ISDIR(entry->st.st_mode)?DT_DIR:S_ISLNK(entry->st.st_mode)?DT_LNK:DT_REG;
  return &dir->entry;
}
void net_closedir(net_dir_t *dir) {
  pthread_mutex_lock(&registry_lock);
  net_dir_t **p=&directories; while(*p && *p!=dir)p=&(*p)->next;
  if(*p)*p=dir->next;
  pthread_mutex_unlock(&registry_lock); free(dir->items); free(dir);
}
int net_stat(const char *path,struct stat *st) {
  char parent[PATH_MAX];
  if(path_dirname(path,parent,sizeof(parent))) { errno=ENAMETOOLONG; return -1; }
  pthread_mutex_lock(&registry_lock);
  for(net_dir_t *dir=directories;dir;dir=dir->next) {
    if(!strcmp(dir->path,path)) { *st=dir->self; pthread_mutex_unlock(&registry_lock); return 0; }
    if(strcmp(dir->path,parent))continue;
    /* The caller normally stats the entry just returned by readdir, as in SMB. */
    size_t first=dir->position?dir->position-1:0;
    for(size_t n=0;n<dir->count;n++) {
      size_t i=(first+n)%dir->count;
      char child[PATH_MAX];
      if(!net_join(child,sizeof(child),parent,dir->items[i].name) && !strcmp(child,path)) {
      *st=dir->items[i].st; pthread_mutex_unlock(&registry_lock); return 0;
      }
    }
  }
  pthread_mutex_unlock(&registry_lock);
  smb_address_t auth;
  if(storage_remote_address(path,&auth))return -1;
  net_dir_t dir={0};
  if(!ftp_path(path)) {
    strcpy(dir.path,path); int result=dav_list(&dir,&auth,0,NULL);
    if(!result)*st=dir.self;
    free(dir.items); return result;
  }
  if(!strcmp(auth.path,path)) {
    strcpy(dir.path,path); int result=ftp_list(&dir,&auth);
    if(!result)*st=dir.self;
    free(dir.items); return result;
  }
  strcpy(dir.path,parent);
  int result=ftp_list(&dir,&auth);
  if(!result) {
    result=-1; errno=ENOENT;
    for(size_t i=0;i<dir.count;i++) {
      char child[PATH_MAX];
      if(!net_join(child,sizeof(child),parent,dir.items[i].name) && !strcmp(child,path)) { *st=dir.items[i].st; result=0; break; }
    }
  }
  int error=errno; free(dir.items); errno=error; return result;
}
int net_statvfs(const char *path,struct statvfs *st) {
  if(ftp_path(path)) { errno=ENOTSUP; return -1; }
  smb_address_t auth; if(storage_remote_address(path,&auth))return -1;
  net_dir_t dir={0}; strcpy(dir.path,path); memset(st,0,sizeof(*st));
  int result=dav_list(&dir,&auth,0,st); free(dir.items);
  if(!result && !st->f_frsize) { errno=ENOTSUP; return -1; }
  return result;
}

int net_fd(int fd) { return fd>=NET_FD_BASE && fd<NET_FD_BASE+NET_FILES; }
static net_file_t *file_for(int fd) {
  pthread_mutex_lock(&registry_lock);
  net_file_t *file=net_fd(fd)?files[fd-NET_FD_BASE]:NULL;
  pthread_mutex_unlock(&registry_lock);
  if(!file)errno=EBADF;
  return file;
}
typedef struct range_body { unsigned char *data; size_t size,used; off_t offset; int accepted,partial,error; } range_body_t;
static size_t range_header(char *data,size_t size,size_t count,void *opaque) {
  range_body_t *body=opaque; size*=count;
  if(size>=5 && !memcmp(data,"HTTP/",5)) {
    char line[80]; size_t n=size<sizeof(line)-1?size:sizeof(line)-1;
    memcpy(line,data,n); line[n]=0;
    int code=0; (void)sscanf(line,"HTTP/%*s %d",&code);
    body->partial=code==206;
    body->accepted=code==200 && !body->offset;
  } else if(body->partial && size>14 && !strncasecmp(data,"Content-Range:",14)) {
    char line[128]; size_t n=size<sizeof(line)-1?size:sizeof(line)-1;
    memcpy(line,data,n); line[n]=0;
    uint64_t start=0,end=0;
    body->accepted=sscanf(line+14," bytes %" SCNu64 "-%" SCNu64,&start,&end)==2 &&
      start==(uint64_t)body->offset && end>=start && end-start==body->size-1;
  }
  return size;
}
static size_t range_write(char *data,size_t size,size_t count,void *opaque) {
  range_body_t *body=opaque; size*=count;
  if(!body->accepted) { body->error=ENOTSUP; return 0; }
  if(size>body->size-body->used) { body->error=EIO; return 0; }
  memcpy(body->data+body->used,data,size); body->used+=size; return size;
}
static ssize_t read_range(net_file_t *file,void *data,size_t size,off_t offset) {
  if(offset>=file->st.st_size)return 0;
  if((uint64_t)size>(uint64_t)(file->st.st_size-offset))size=(size_t)(file->st.st_size-offset);
  range_body_t body={.data=data,.size=size,.offset=offset,.accepted=ftp_path(file->path)};
  char range[80]; snprintf(range,sizeof(range),"%" PRIu64 "-%" PRIu64,(uint64_t)offset,(uint64_t)offset+size-1);
  if(ftp_path(file->path) && offset+(off_t)size==file->st.st_size)
    snprintf(range,sizeof(range),"%" PRIu64 "-",(uint64_t)offset);
  curl_easy_setopt(file->curl,CURLOPT_RANGE,range);
  curl_easy_setopt(file->curl,CURLOPT_WRITEFUNCTION,range_write);
  curl_easy_setopt(file->curl,CURLOPT_WRITEDATA,&body);
  curl_easy_setopt(file->curl,CURLOPT_HEADERFUNCTION,range_header);
  curl_easy_setopt(file->curl,CURLOPT_HEADERDATA,&body);
  int result=perform(file->curl,0);
  if(body.error) { errno=body.error; return -1; }
#if WFM_DEBUG
  long status=0; curl_easy_getinfo(file->curl,CURLINFO_RESPONSE_CODE,&status);
  if(result || body.used!=size)
    fprintf(stderr,"Remote read: path=%s offset=%lld expected=%zu received=%zu status=%ld curl=%d\n",
            file->path,(long long)offset,size,body.used,status,(int)last_curl_error);
#endif
  if(result)return -1;
  if(body.used!=size) { errno=EIO; return -1; }
  return (ssize_t)body.used;
}
ssize_t net_pread(int fd,void *data,size_t size,off_t offset) {
  net_file_t *file=file_for(fd); if(!file)return -1;
  if(offset<0 || file->writing) { errno=EINVAL; return -1; }
  if(!size)return 0;
  pthread_mutex_lock(&file->lock);
  ssize_t result;
  if(offset>=file->cache_offset && (uint64_t)(offset-file->cache_offset)<file->cache_size) {
    size_t part=file->cache_size-(size_t)(offset-file->cache_offset); if(part>size)part=size;
    memcpy(data,file->cache+(size_t)(offset-file->cache_offset),part); result=(ssize_t)part;
  } else {
    size_t capacity=transfer_use_pipeline((uint64_t)file->st.st_size)?TRANSFER_READ_BUFFER_SIZE:65536;
    if(size>=capacity)result=read_range(file,data,size,offset);
    else {
      if(!file->cache) { file->cache=malloc(capacity); file->cache_capacity=capacity; }
      if(!file->cache)result=read_range(file,data,size,offset);
      else {
        result=read_range(file,file->cache,file->cache_capacity,offset);
        if(result>=0) {
          file->cache_offset=offset; file->cache_size=(size_t)result;
          if((size_t)result>size)result=(ssize_t)size;
          memcpy(data,file->cache,(size_t)result);
        }
      }
    }
  }
  pthread_mutex_unlock(&file->lock); return result;
}
ssize_t net_read(int fd,void *data,size_t size) {
  net_file_t *file=file_for(fd); if(!file)return -1;
  ssize_t result=net_pread(fd,data,size,file->position);
  if(result>0)file->position+=result;
  return result;
}
static size_t upload_read(char *data,size_t size,size_t count,void *opaque) {
  net_file_t *file=opaque; size*=count;
  pthread_mutex_lock(&file->lock);
  while(!file->used && !file->done)pthread_cond_wait(&file->ready,&file->lock);
  if(!file->used) { pthread_mutex_unlock(&file->lock); return 0; }
  size_t part=file->capacity-file->head;
  if(part>file->used)part=file->used;
  if(part>size)part=size;
  memcpy(data,file->ring+file->head,part);
  file->head=(file->head+part)%file->capacity; file->used-=part;
  pthread_cond_signal(&file->space); pthread_mutex_unlock(&file->lock); return part;
}
static void *upload_worker(void *opaque) {
  net_file_t *file=opaque;
  int result=0,error=0;
  if(!ftp_path(file->path) && file->auth.user[0]) {
    /* Negotiate authentication before consuming a non-seekable upload stream. */
    char parent[PATH_MAX],url[PATH_MAX];
    if(path_dirname(file->path,parent,sizeof(parent)) || transport_url(parent,url,sizeof(url))) {
      result=-1; error=errno; goto finished;
    }
    size_t url_length=strlen(url);
    if(url_length+1>=sizeof(url)) { result=-1; error=ENAMETOOLONG; goto finished; }
    if(url[url_length-1]!='/') { url[url_length++]='/'; url[url_length]=0; }
    curl_easy_setopt(file->curl,CURLOPT_URL,url);
    struct curl_slist *headers=curl_slist_append(NULL,"Depth: 0");
    curl_easy_setopt(file->curl,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(file->curl,CURLOPT_POSTFIELDS,"");
    curl_easy_setopt(file->curl,CURLOPT_CUSTOMREQUEST,"PROPFIND");
    result=perform(file->curl,1);
    curl_easy_setopt(file->curl,CURLOPT_HTTPHEADER,NULL);
    curl_easy_setopt(file->curl,CURLOPT_POSTFIELDS,NULL);
    curl_easy_setopt(file->curl,CURLOPT_CUSTOMREQUEST,NULL);
    curl_slist_free_all(headers);
    if(result) { error=errno; goto finished; }
    if(transport_url(file->path,url,sizeof(url))) { result=-1; error=errno; goto finished; }
    curl_easy_setopt(file->curl,CURLOPT_URL,url);
  }
  curl_easy_setopt(file->curl,CURLOPT_UPLOAD,1L);
  curl_easy_setopt(file->curl,CURLOPT_UPLOAD_BUFFERSIZE,
                   transfer_use_pipeline(file->expected>0?(uint64_t)file->expected:0)?(long)TRANSFER_HTTP_BUFFER_SIZE:65536L);
  curl_easy_setopt(file->curl,CURLOPT_INFILESIZE_LARGE,file->expected);
  curl_easy_setopt(file->curl,CURLOPT_READFUNCTION,upload_read);
  curl_easy_setopt(file->curl,CURLOPT_READDATA,file);
  result=perform(file->curl,0); error=errno;
  curl_off_t uploaded=0;
  curl_easy_getinfo(file->curl,CURLINFO_SIZE_UPLOAD_T,&uploaded);
  if(!result && file->expected>=0 && uploaded!=file->expected) { result=-1; error=EIO; }
finished:
  pthread_mutex_lock(&file->lock);
  if(!result && file->used) { result=-1; error=EIO; }
  file->error=result?error:0; file->finished=1;
  pthread_cond_broadcast(&file->space); pthread_mutex_unlock(&file->lock);
  return NULL;
}
/* Caller owns file->lock. A bounded producer/consumer stream avoids disk staging. */
static int start_upload(net_file_t *file) {
  if(file->started)return 0;
  file->capacity=transfer_use_pipeline(file->expected>0?(uint64_t)file->expected:0)?TRANSFER_READ_BUFFER_SIZE:262144;
  file->ring=malloc(file->capacity); if(!file->ring)return -1;
  int error=pthread_create(&file->thread,NULL,upload_worker,file);
  if(error) { free(file->ring); file->ring=NULL; errno=error; return -1; }
  file->started=1; return 0;
}
ssize_t net_write(int fd,const void *data,size_t size) {
  net_file_t *file=file_for(fd); if(!file)return -1;
  if(!file->writing) { errno=EBADF; return -1; }
  if(file->expected>=0 && (uint64_t)size>(uint64_t)(file->expected-file->position)) { errno=EFBIG; return -1; }
  pthread_mutex_lock(&file->lock);
  if(file->done || start_upload(file)) { pthread_mutex_unlock(&file->lock); errno=EIO; return -1; }
  size_t copied=0;
  while(copied<size) {
    while(file->used==file->capacity && !file->finished)pthread_cond_wait(&file->space,&file->lock);
    if(file->finished) { errno=file->error?file->error:EIO; pthread_mutex_unlock(&file->lock); return -1; }
    size_t tail=(file->head+file->used)%file->capacity;
    size_t part=file->capacity-tail;
    if(part>file->capacity-file->used)part=file->capacity-file->used;
    if(part>size-copied)part=size-copied;
    memcpy(file->ring+tail,(const char *)data+copied,part); file->used+=part; copied+=part;
    pthread_cond_signal(&file->ready);
  }
  file->position+=(off_t)copied; file->st.st_size=file->position;
  pthread_mutex_unlock(&file->lock); return (ssize_t)copied;
}
int net_fsync(int fd) {
  net_file_t *file=file_for(fd); if(!file)return -1;
  if(!file->writing)return 0;
  pthread_mutex_lock(&file->lock);
  /* Permission probes open existing files without writing or truncating them. */
  if(!file->started && !file->upload_needed) { pthread_mutex_unlock(&file->lock); return 0; }
  if(!file->started && file->expected<0)file->expected=0;
  if(start_upload(file)) { pthread_mutex_unlock(&file->lock); return -1; }
  file->done=1; pthread_cond_broadcast(&file->ready);
  int joined=file->joined; file->joined=1;
  pthread_mutex_unlock(&file->lock);
  if(!joined)pthread_join(file->thread,NULL);
  if(file->error) { errno=file->error; return -1; }
  return 0;
}
int net_open(const char *path,int flags) {
  if((flags&O_ACCMODE)==O_RDWR || (flags&O_APPEND)) { errno=ENOTSUP; return -1; }
  net_file_t *file=calloc(1,sizeof(*file)); if(!file)return -1;
  file->expected=-1;
  file->writing=(flags&O_ACCMODE)==O_WRONLY;
  file->upload_needed=(flags&(O_TRUNC|O_CREAT))!=0;
  if(storage_remote_address(path,&file->auth) || copy_text(file->path,sizeof(file->path),path))goto failed;
  if(!file->writing || (flags&O_EXCL) || !(flags&O_CREAT)) {
    int result=net_stat(path,&file->st);
    if(result && (!file->writing || !(flags&O_CREAT) || errno!=ENOENT))goto failed;
    if(!result && (flags&O_EXCL)) { errno=EEXIST; goto failed; }
    if(!result && !S_ISREG(file->st.st_mode)) { errno=EISDIR; goto failed; }
  }
  file->curl=request_new(path,&file->auth,0); if(!file->curl)goto failed;
  if(!file->writing && transfer_use_pipeline((uint64_t)file->st.st_size))
    curl_easy_setopt(file->curl,CURLOPT_BUFFERSIZE,(long)TRANSFER_HTTP_BUFFER_SIZE);
  if(file->writing) { memset(&file->st,0,sizeof(file->st)); file->st.st_mode=S_IFREG|0777; file->st.st_dev=device_id(path); }
  pthread_mutex_init(&file->lock,NULL); pthread_cond_init(&file->ready,NULL); pthread_cond_init(&file->space,NULL);
  pthread_mutex_lock(&registry_lock);
  for(int i=0;i<NET_FILES;i++)if(!files[i]) { files[i]=file; pthread_mutex_unlock(&registry_lock); return NET_FD_BASE+i; }
  pthread_mutex_unlock(&registry_lock);
  pthread_mutex_destroy(&file->lock); pthread_cond_destroy(&file->ready); pthread_cond_destroy(&file->space); errno=EMFILE;
failed: {
  int error=errno; if(file->curl)curl_easy_cleanup(file->curl); memset(&file->auth,0,sizeof(file->auth)); free(file); errno=error; return -1;
}
}
int net_close(int fd) {
  net_file_t *file=file_for(fd); if(!file)return -1;
  int result=net_fsync(fd),error=errno;
  pthread_mutex_lock(&registry_lock); files[fd-NET_FD_BASE]=NULL; pthread_mutex_unlock(&registry_lock);
  curl_easy_cleanup(file->curl); free(file->cache); free(file->ring);
  pthread_mutex_destroy(&file->lock); pthread_cond_destroy(&file->ready); pthread_cond_destroy(&file->space);
  memset(file,0,sizeof(*file)); free(file); errno=error; return result;
}
int net_fstat(int fd,struct stat *st) { net_file_t *file=file_for(fd); if(!file)return -1; *st=file->st; return 0; }
void net_set_size(int fd,uint64_t size) { net_file_t *file=file_for(fd); if(file && file->writing && !file->started)file->expected=(curl_off_t)size; }
off_t net_lseek(int fd,off_t offset,int whence) {
  net_file_t *file=file_for(fd); if(!file)return -1;
  if(file->writing && (offset || whence!=SEEK_CUR)) { errno=ENOTSUP; return -1; }
  off_t base=whence==SEEK_SET?0:whence==SEEK_CUR?file->position:whence==SEEK_END?file->st.st_size:-1;
  if(base<0 || offset < -base || (offset>0 && base>INT64_MAX-offset)) { errno=EINVAL; return -1; }
  file->position=base+offset; return file->position;
}
static int path_command(const char *path,const char *dav,const char *ftp,int directory,const char *destination) {
  smb_address_t auth; if(storage_remote_address(path,&auth))return -1;
  char origin[PATH_MAX];
  const char *operation_path=path;
  if(ftp_path(path)) {
    const char *slash=strchr(strstr(path,"://")+3,'/');
    size_t length=slash?(size_t)(slash-path):strlen(path);
    memcpy(origin,path,length); origin[length]=0; operation_path=origin;
  }
  CURL *curl=request_new(operation_path,&auth,directory); if(!curl)return -1;
  struct curl_slist *headers=NULL,*commands=NULL;
  int result=-1,exists=0;
  if(destination) {
    struct stat st;
    exists=!net_stat(destination,&st);
    if(exists && S_ISDIR(st.st_mode)) { errno=EEXIST; goto done; }
    if(!exists && errno!=ENOENT)goto done;
  }
  if(ftp_path(path)) {
    const char *relative=strchr(strstr(path,"://")+3,'/'); if(!relative)relative="/";
    char command[PATH_MAX+16];
    commands=curl_slist_append(commands,ftp_utf8.data);
    char *decoded=curl_easy_unescape(NULL,relative[0]=='/'?relative+1:relative,0,NULL);
    if(!decoded || !clean_component(decoded)) { curl_free(decoded); errno=EINVAL; goto done; }
    snprintf(command,sizeof(command),"%s %s",ftp,decoded); curl_free(decoded);
    commands=curl_slist_append(commands,command);
    if(destination) {
      const char *target=strchr(strstr(destination,"://")+3,'/');
      decoded=curl_easy_unescape(NULL,target?target+1:"",0,NULL);
      if(!decoded || !clean_component(decoded)) { curl_free(decoded); errno=EINVAL; goto done; }
      snprintf(command,sizeof(command),"RNTO %s",decoded); curl_free(decoded); commands=curl_slist_append(commands,command);
    }
    curl_easy_setopt(curl,CURLOPT_QUOTE,commands);
    curl_easy_setopt(curl,CURLOPT_NOBODY,1L);
  } else {
    curl_easy_setopt(curl,CURLOPT_CUSTOMREQUEST,dav);
    curl_easy_setopt(curl,CURLOPT_POSTFIELDS,"");
    if(destination) {
      CURL *target=request_new(destination,&auth,0); if(!target)goto done;
      char *url=NULL; curl_easy_getinfo(target,CURLINFO_EFFECTIVE_URL,&url);
      char header[PATH_MAX*3+32]; snprintf(header,sizeof(header),"Destination: %s",url);
      headers=curl_slist_append(headers,header);
      curl_easy_cleanup(target);
      /* Atomic text saves and completed uploads replace files, never collections. */
      headers=curl_slist_append(headers,exists?"Overwrite: T":"Overwrite: F");
    }
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  }
  result=perform(curl,1);
done: {
  int error=errno; curl_slist_free_all(headers); curl_slist_free_all(commands); curl_easy_cleanup(curl); errno=error; return result;
}
}
int net_mkdir(const char *path) {
  int result=path_command(path,"MKCOL","MKD",1,NULL);
  if(result && (errno==EACCES || errno==ENOTSUP)) {
    int error=errno; struct stat st;
    errno=!net_stat(path,&st)?EEXIST:error;
  }
  return result;
}
int net_remove(const char *path,int directory) { return path_command(path,"DELETE",directory?"RMD":"DELE",directory,NULL); }
int net_rename_compatible(const char *source,const char *destination) {
  if(!net_path(source) || !net_path(destination))return 0;
  smb_address_t a,b;
  if(storage_remote_address(source,&a) || storage_remote_address(destination,&b))return -1;
  return device_id(source)==device_id(destination) && !strcmp(a.server,b.server) &&
    !strcmp(a.user,b.user) && !strcmp(a.password,b.password);
}
int net_rename(const char *source,const char *destination) {
  int compatible=net_rename_compatible(source,destination);
  if(compatible<=0) { if(!compatible)errno=EXDEV; return -1; }
  return path_command(source,"MOVE","RNFR",0,destination);
}
