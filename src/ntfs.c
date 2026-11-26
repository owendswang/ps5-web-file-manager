/* NTFS-3G adapter. GPL-3.0-or-later; uses upstream GPL-2.0-or-later APIs. */
#define _GNU_SOURCE
#include "ntfs.h"
#include "path_util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#else
#include <sys/disk.h>
#include <sys/mount.h>
#endif

#include <ntfs-3g/attrib.h>
#include <ntfs-3g/cache.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/logging.h>
#include <ntfs-3g/logfile.h>
#include <ntfs-3g/ntfstime.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/volume.h>

#define NTFS_VOLUMES 32
#define NTFS_FILES 64
#define NTFS_DISKS 16
#define NTFS_IDLE_SECONDS 30
#define NTFS_FD_BASE 0x60000000
#define NTFS_IO_BLOCK (256 * 1024)
typedef struct {
  char root[48], device[PATH_MAX];
  uint64_t offset, length, serial;
  unsigned sector, disk;
  int fd, refs, present;
  const char *format_error;
  const char *read_only_code;
  int write_checked;
  uint64_t free_bytes, total_bytes;
  int space_known;
  time_t used;
  off_t position;
  ntfs_volume *volume;
} usb_volume_t;
typedef struct {
  usb_volume_t *volume;
  ntfs_inode *inode;
  ntfs_attr *data;
  off_t position;
  int flags;
} usb_file_t;
typedef struct {
  char *name;
  ino_t inode;
  unsigned type;
} usb_entry_t;
struct usbntfs_dir {
  usb_entry_t *entries;
  size_t count, index, capacity;
  struct dirent entry;
};
static usb_volume_t volumes[NTFS_VOLUMES];
static usb_file_t files[NTFS_FILES];
static struct stat devices[NTFS_DISKS];
static size_t volume_count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static time_t scanned;
static _Thread_local const char *last_error;
static int cleanup_registered;

const char *usbntfs_error(void) { return last_error; }
void usbntfs_clear_error(void) { last_error = NULL; }
static int fail(const char *code, int error) { last_error = code; errno = error; return -1; }
int usbntfs_path(const char *path) {
  if(!path || strncmp(path,"/mnt/ntfs",9))return 0;
  const char *p=path+9;
  if(*p<'0' || *p>'9')return 0;
  while(*p>='0' && *p<='9')p++;
  if(*p++!='p' || *p<'0' || *p>'9')return 0;
  while(*p>='0' && *p<='9')p++;
  return !*p || *p=='/';
}
int usbntfs_fd(int fd) { return fd>=NTFS_FD_BASE && fd<NTFS_FD_BASE+NTFS_FILES; }
static uint32_t le32_at(const unsigned char *p) {
  return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint64_t le64_at(const unsigned char *p) { return le32_at(p) | (uint64_t)le32_at(p+4)<<32; }

static int same_device(const struct stat *a,const struct stat *b) {
  return a->st_mode==b->st_mode && a->st_dev==b->st_dev &&
    a->st_ino==b->st_ino && a->st_rdev==b->st_rdev;
}

static void remember_capacity(usb_volume_t *v) {
  if(v->volume && NVolFreeSpaceKnown(v->volume)) {
    v->free_bytes=(uint64_t)v->volume->free_clusters*v->volume->cluster_size;
    v->total_bytes=(uint64_t)v->volume->nr_clusters*v->volume->cluster_size;
    v->space_known=1;
  }
}

static void close_volume(usb_volume_t *v) {
  if(!v->volume)return;
  if(v->present)remember_capacity(v);
  ntfs_umount(v->volume,FALSE);
  v->volume=NULL;
}

void usbntfs_reap_idle(void) {
  if(pthread_mutex_trylock(&lock))return;
  time_t now=time(NULL);
  for(size_t i=0;i<volume_count;i++) {
    usb_volume_t *v=&volumes[i];
    if(!v->refs && (!v->present || now-v->used>=NTFS_IDLE_SECONDS))close_volume(v);
  }
  pthread_mutex_unlock(&lock);
}

/* Native USB devices require aligned offsets and lengths. All I/O is bounded
   to the selected partition, including read/modify/write of partial sectors. */
static ssize_t device_io(usb_volume_t *v, void *buffer, size_t count, uint64_t at, int writing) {
  if(at>v->length || count>v->length-at) { errno=EINVAL; return -1; }
  unsigned char *data=buffer;
  size_t done=0;
  void *bounce=NULL;
  if(posix_memalign(&bounce,4096,NTFS_IO_BLOCK+4096)) { errno=ENOMEM; return -1; }
  while(done<count) {
    uint64_t absolute=v->offset+at+done;
    size_t skip=absolute%v->sector;
    size_t n=count-done;
    if(n>NTFS_IO_BLOCK-skip)n=NTFS_IO_BLOCK-skip;
    size_t aligned=(skip+n+v->sector-1)/v->sector*v->sector;
    ssize_t got;
    if(!writing || skip || n!=aligned) {
      do { got=pread(v->fd,bounce,aligned,(off_t)(absolute-skip)); } while(got<0 && errno==EINTR);
      if(got!=(ssize_t)aligned) { if(got>=0)errno=EIO; free(bounce); return -1; }
    }
    if(writing) {
      memcpy((char *)bounce+skip,data+done,n);
      do { got=pwrite(v->fd,bounce,aligned,(off_t)(absolute-skip)); } while(got<0 && errno==EINTR);
      if(got!=(ssize_t)aligned) { if(got>=0)errno=EIO; free(bounce); return -1; }
    } else memcpy(data+done,(char *)bounce+skip,n);
    done+=n;
  }
  free(bounce);
  return (ssize_t)done;
}
static int dev_open(struct ntfs_device *d,int flags) {
  usb_volume_t *v=d->d_private;
  v->fd=open(v->device,flags);
  if(v->fd<0)return -1;
  struct stat current;
  if(fstat(v->fd,&current) || !same_device(&current,&devices[v->disk])) {
    close(v->fd); v->fd=-1; errno=ENODEV; return -1;
  }
  NDevSetOpen(d);
  if((flags&O_ACCMODE)==O_RDONLY)NDevSetReadOnly(d);
  v->position=0;
  return 0;
}
static int dev_close(struct ntfs_device *d) {
  usb_volume_t *v=d->d_private;
  int result=close(v->fd); v->fd=-1; NDevClearOpen(d); return result;
}
static s64 dev_pread(struct ntfs_device *d,void *b,s64 n,s64 p) {
  if(n<0 || p<0) { errno=EINVAL; return -1; }
  return device_io(d->d_private,b,(size_t)n,(uint64_t)p,0);
}
static s64 dev_pwrite(struct ntfs_device *d,const void *b,s64 n,s64 p) {
  if(NDevReadOnly(d)) { errno=EROFS; return -1; }
  if(n<0 || p<0) { errno=EINVAL; return -1; }
  NDevSetDirty(d);
  return device_io(d->d_private,(void *)b,(size_t)n,(uint64_t)p,1);
}
static s64 dev_seek(struct ntfs_device *d,s64 p,int whence) {
  usb_volume_t *v=d->d_private;
  s64 base=whence==SEEK_SET?0:whence==SEEK_CUR?v->position:whence==SEEK_END?(s64)v->length:-1;
  if(base<0 || p < -base || (p>0 && base>INT64_MAX-p)) { errno=EINVAL; return -1; }
  v->position=base+p; return v->position;
}
static s64 dev_read(struct ntfs_device *d,void *b,s64 n) {
  usb_volume_t *v=d->d_private; s64 got=dev_pread(d,b,n,v->position);
  if(got>0)v->position+=got;
  return got;
}
static s64 dev_write(struct ntfs_device *d,const void *b,s64 n) {
  usb_volume_t *v=d->d_private; s64 got=dev_pwrite(d,b,n,v->position);
  if(got>0)v->position+=got;
  return got;
}
static int dev_sync(struct ntfs_device *d) {
  usb_volume_t *v=d->d_private;
  if(NDevReadOnly(d) || !NDevDirty(d))return 0;
  if(fsync(v->fd))return -1;
  NDevClearDirty(d);
  return 0;
}
static int dev_stat(struct ntfs_device *d,struct stat *st) {
  usb_volume_t *v=d->d_private;
  if(fstat(v->fd,st))return -1;
  st->st_size=(off_t)v->length; return 0;
}
static int dev_ioctl(struct ntfs_device *d,unsigned long request,void *arg) {
  usb_volume_t *v=d->d_private;
#ifdef __linux__
  if(request==BLKSSZGET) { *(int *)arg=v->sector; return 0; }
  if(request==BLKGETSIZE64) { *(uint64_t *)arg=v->length; return 0; }
#else
  if(request==DIOCGSECTORSIZE) { *(unsigned *)arg=v->sector; return 0; }
  if(request==DIOCGMEDIASIZE) { *(off_t *)arg=(off_t)v->length; return 0; }
#endif
  errno=ENOTTY; return -1;
}
static struct ntfs_device_operations device_ops={dev_open,dev_close,dev_seek,
  dev_read,dev_write,dev_pread,dev_pwrite,dev_sync,dev_stat,dev_ioctl};

static void add_partition(const char *device,unsigned disk,unsigned part,
                          unsigned sector,uint64_t offset,uint64_t length,
                          const unsigned char *boot) {
  char root[48]; snprintf(root,sizeof(root),"/mnt/ntfs%up%u",disk,part);
  size_t i;
  for(i=0;i<volume_count && strcmp(volumes[i].root,root);i++);
  if(i==NTFS_VOLUMES)return;
  if(i==volume_count) {
    volume_count++;
    snprintf(volumes[i].root,sizeof(volumes[i].root),"%s",root);
    snprintf(volumes[i].device,sizeof(volumes[i].device),"%s",device);
    volumes[i].fd=-1;
  }
  usb_volume_t *v=&volumes[i];
  uint64_t serial=le64_at(boot+72);
  if(v->volume && (v->offset!=offset || v->length!=length || v->serial!=serial || strcmp(v->device,device))) {
    if(v->refs)return;
    close_volume(v);
  }
  if(v->offset!=offset || v->length!=length || v->serial!=serial || strcmp(v->device,device)) {
    v->space_known=0;
    v->write_checked=0;
    v->read_only_code=NULL;
  }
  snprintf(v->device,sizeof(v->device),"%s",device);
  v->offset=offset; v->length=length; v->serial=serial; v->sector=sector; v->disk=disk; v->present=1;
  v->format_error=!memcmp(boot+3,"NTFS    ",8)?NULL:
    !memcmp(boot+3,"-FVE-FS-",8)?"ntfs_encrypted":"ntfs_format_unsupported";
}
static void probe_partition(usb_volume_t *disk,unsigned index,unsigned part,uint64_t start,uint64_t length) {
  unsigned char boot[512];
  if(start>disk->length || length>disk->length-start || length<512)return;
  if(device_io(disk,boot,sizeof(boot),start,0)<0)return;
  /* Native FAT/exFAT storage remains entirely with the system driver. */
  if(!memcmp(boot+3,"EXFAT   ",8) || !memcmp(boot+54,"FAT",3) || !memcmp(boot+82,"FAT",3))return;
  add_partition(disk->device,index,part,disk->sector,start,length,boot);
}
static void scan_extended(usb_volume_t *disk,unsigned index,uint64_t base,uint64_t length) {
  uint64_t next=base,seen[NTFS_VOLUMES];
  if(base>disk->length || length>disk->length-base)return;
  for(unsigned i=0;i<NTFS_VOLUMES;i++) {
    for(unsigned k=0;k<i;k++)if(seen[k]==next)return;
    seen[i]=next;
    unsigned char ebr[512];
    if(next<base || next-base>=length || device_io(disk,ebr,sizeof(ebr),next,0)<0 || ebr[510]!=0x55 || ebr[511]!=0xaa)return;
    uint64_t relative=(uint64_t)le32_at(ebr+454)*disk->sector;
    uint64_t bytes=(uint64_t)le32_at(ebr+458)*disk->sector;
    if(relative<=length-(next-base) && bytes<=length-(next-base)-relative && ebr[450])
      probe_partition(disk,index,i+5,next+relative,bytes);
    unsigned char *entry=ebr+462;
    if(entry[4]!=5 && entry[4]!=0x0f && entry[4]!=0x85)return;
    relative=(uint64_t)le32_at(entry+8)*disk->sector;
    if(!relative || relative>=length)return;
    next=base+relative;
  }
}
static void scan_device(const char *path,unsigned index) {
  usb_volume_t disk={.fd=open(path,O_RDONLY),.sector=512};
  if(disk.fd<0)return;
  snprintf(disk.device,sizeof(disk.device),"%s",path);
  struct stat st;
  if(fstat(disk.fd,&st))goto done;
  if(S_ISREG(st.st_mode))disk.length=(uint64_t)st.st_size;
  else {
#ifdef __linux__
    if(ioctl(disk.fd,BLKSSZGET,&disk.sector) || ioctl(disk.fd,BLKGETSIZE64,&disk.length))goto done;
#else
    off_t length;
    if(ioctl(disk.fd,DIOCGSECTORSIZE,&disk.sector) || ioctl(disk.fd,DIOCGMEDIASIZE,&length))goto done;
    disk.length=(uint64_t)length;
#endif
  }
  if(disk.sector<512 || disk.sector>4096 || (disk.sector&(disk.sector-1)) || disk.length>INT64_MAX)goto done;
  unsigned char block[4096];
  if(device_io(&disk,block,512,0,0)<0)goto done;
  if(!memcmp(block+3,"EXFAT   ",8) || !memcmp(block+54,"FAT",3) || !memcmp(block+82,"FAT",3))goto done;
  if(!memcmp(block+3,"NTFS    ",8) || !memcmp(block+3,"-FVE-FS-",8)) {
    probe_partition(&disk,index,0,0,disk.length); goto done;
  }
  if(block[510]!=0x55 || block[511]!=0xaa) {
    probe_partition(&disk,index,0,0,disk.length); goto done;
  }
  int gpt=0;
  for(unsigned p=0;p<4;p++)if(block[446+p*16+4]==0xee)gpt=1;
  if(gpt) {
    if(device_io(&disk,block,disk.sector,disk.sector,0)<0 || memcmp(block,"EFI PART",8))goto done;
    uint64_t table=le64_at(block+72);
    unsigned count=le32_at(block+80),stride=le32_at(block+84);
    if(stride<128 || stride>4096 || count>4096 || table>disk.length/disk.sector)goto done;
    uint64_t pos=table*disk.sector;
    for(unsigned p=0;p<count && p<NTFS_VOLUMES;p++) {
      unsigned char entry[128];
      if((uint64_t)p*stride>disk.length-pos || device_io(&disk,entry,sizeof(entry),pos+(uint64_t)p*stride,0)<0)break;
      unsigned nonzero=0; for(unsigned k=0;k<16;k++)nonzero|=entry[k];
      if(!nonzero)continue;
      uint64_t start=le64_at(entry+32),end=le64_at(entry+40);
      if(end<start || end>=disk.length/disk.sector)continue;
      probe_partition(&disk,index,p+1,start*disk.sector,(end-start+1)*disk.sector);
    }
  } else {
    for(unsigned p=0;p<4;p++) {
      unsigned char *entry=block+446+p*16;
      if(!entry[4])continue;
      uint64_t start=(uint64_t)le32_at(entry+8)*disk.sector;
      uint64_t length=(uint64_t)le32_at(entry+12)*disk.sector;
      if(entry[4]==5 || entry[4]==0x0f || entry[4]==0x85) {
        scan_extended(&disk,index,start,length); continue;
      }
      probe_partition(&disk,index,p+1,start,length);
    }
  }
done:
  close(disk.fd);
}
/* A device-node stat checks attachment without opening or reading the disk. */
static void check_device(const char *path,unsigned disk,int force) {
  struct stat current={0};
  if(stat(path,&current))memset(&current,0,sizeof(current));
  struct stat *previous=&devices[disk];
  int changed=!same_device(&current,previous);
  if(!changed && !force)return;

  int busy=0;
  for(size_t i=0;i<volume_count;i++) {
    usb_volume_t *v=&volumes[i];
    if(v->disk!=disk)continue;
    if(v->refs)busy=1;
    if(changed) {
      v->present=0;
      v->space_known=0;
      if(!v->refs)close_volume(v);
      v->write_checked=0;
      v->read_only_code=NULL;
    }
  }
  /* Keep old descriptors bound to their original volume until they close. */
  if(busy) {
    if(changed)memset(previous,0,sizeof(*previous));
    return;
  }

  *previous=current;
  if(!current.st_mode)return;
  for(size_t i=0;i<volume_count;i++)
    if(volumes[i].disk==disk)volumes[i].present=0;
  scan_device(path,disk);
}

static void scan_locked(int force) {
  time_t now=time(NULL);
  if(!force && scanned && now-scanned<2)return;
  scanned=now;
  if(!cleanup_registered) { atexit(usbntfs_shutdown); cleanup_registered=1; }
#ifdef __linux__
  /* Explicit disk images only on hosts; never scan the developer's real disks. */
  const char *input=getenv("WFM_NTFS_IMAGES");
  char *copy=input?strdup(input):NULL,*save=NULL;
  unsigned disk=0;
  for(char *p=copy?strtok_r(copy,":",&save):NULL;p && disk<NTFS_DISKS;p=strtok_r(NULL,":",&save))
    check_device(p,disk++,force);
  free(copy);
#else
  for(unsigned i=0;i<NTFS_DISKS;i++) {
    char path[32]; snprintf(path,sizeof(path),"/dev/da%u",i); check_device(path,i,force);
  }
#endif
}
static ntfs_volume *mount_volume(usb_volume_t *v,ntfs_mount_flags flags) {
  struct ntfs_device *device=ntfs_device_alloc(v->device,0,&device_ops,v);
  if(!device)return NULL;
  ntfs_volume *volume=ntfs_device_mount(device,flags);
  if(!volume) { int error=errno; ntfs_device_free(device); errno=error; }
  return volume;
}

static ntfs_volume *inspect_volume(usb_volume_t *v) {
  ntfs_set_char_encoding("UTF-8");
#if !WFM_DEBUG
  ntfs_log_set_handler(ntfs_log_handler_null);
#endif
  ntfs_volume *volume=mount_volume(v,NTFS_MNT_RDONLY|NTFS_MNT_FORENSIC);
  v->write_checked=volume!=NULL;
  v->read_only_code=NULL;
  if(volume) {
    if(volume->flags&VOLUME_IS_DIRTY) {
      v->read_only_code="ntfs_readonly_dirty";
    } else if(ntfs_volume_check_hiberfile(volume,0)<0 || ntfs_volume_check_logfile(volume)<0) {
      v->read_only_code="ntfs_readonly_unsafe";
    }
  }
  return volume;
}

static int require_writable(usb_volume_t *v) {
  if(!NVolReadOnly(v->volume))return 0;
  return fail(v->read_only_code?v->read_only_code:"ntfs_readonly",EROFS);
}

static usb_volume_t *find_volume(const char *path) {
  for(size_t i=0;i<volume_count;i++) {
    usb_volume_t *v=&volumes[i]; size_t n=strlen(v->root);
    if(!strncmp(path,v->root,n) && (!path[n] || path[n]=='/'))return v;
  }
  return NULL;
}

int usbntfs_present(const char *path) {
  pthread_mutex_lock(&lock);
  usb_volume_t *v=find_volume(path);
  int present=v && v->present && !v->format_error;
  pthread_mutex_unlock(&lock);
  return present;
}

static usb_volume_t *get_volume(const char *path,const char **relative) {
  scan_locked(0);
  usb_volume_t *v=find_volume(path);
  if(v) {
    if(!v->present) { fail("ntfs_disconnected",ENODEV); return NULL; }
    if(v->format_error) { fail(v->format_error,EOPNOTSUPP); return NULL; }
    v->used=time(NULL);
    const char *rel=path+strlen(v->root);
    if(rel[0] && rel[1] && !relative_path_safe(rel+1)) { errno=EINVAL; return NULL; }
    *relative=*rel?rel:"/";
    if(v->volume)return v;
    /* Inspect with O_RDONLY first: a writable mount can update metadata. */
    v->volume=inspect_volume(v);
    if(!v->volume) {
      int error=errno;
      fail(error==ENODEV || error==ENOENT?"ntfs_disconnected":
           error==EPERM || error==EACCES?"ntfs_unclean":
           error==EOPNOTSUPP?"ntfs_unsupported":"ntfs_invalid",error);
      return NULL;
    }
    if(!v->read_only_code) {
      ntfs_umount(v->volume,FALSE); v->volume=NULL;
      /* No RECOVER or IGNORE_HIBERFILE; retain upstream's final checks. */
      v->volume=mount_volume(v,NTFS_MNT_NONE);
      if(!v->volume)v->volume=mount_volume(v,NTFS_MNT_RDONLY|NTFS_MNT_FORENSIC);
      if(!v->volume) { v->write_checked=0; fail("ntfs_invalid",errno); return NULL; }
      if(NVolReadOnly(v->volume))v->read_only_code="ntfs_readonly";
    }
    ntfs_create_lru_caches(v->volume);
    NVolSetCompression(v->volume);
    ntfs_set_shown_files(v->volume,FALSE,TRUE,FALSE);
    return v;
  }
  fail("ntfs_disconnected",ENODEV); return NULL;
}
static ntfs_inode *lookup(usb_volume_t *v,const char *relative,int writing) {
  if(writing && require_writable(v))return NULL;
  ntfs_inode *ni=ntfs_pathname_to_inode(v->volume,NULL,relative);
  if(!ni)return NULL;
  if((ni->flags&(FILE_ATTR_REPARSE_POINT|FILE_ATTR_ENCRYPTED)) ||
     (writing && ni->mft_no<FILE_first_user)) {
    ntfs_inode_close(ni); fail("ntfs_unsupported",EOPNOTSUPP); return NULL;
  }
  return ni;
}
static void inode_stat(usb_volume_t *v,ntfs_inode *ni,struct stat *st) {
  memset(st,0,sizeof(*st));
  st->st_dev=(dev_t)(0x70000000u+(unsigned)(v-volumes));
  st->st_ino=ni->mft_no;
  st->st_nlink=le16_to_cpu(ni->mrec->link_count);
  st->st_mode=(ni->flags&FILE_ATTR_REPARSE_POINT?S_IFLNK:
               ni->mrec->flags&MFT_RECORD_IS_DIRECTORY?S_IFDIR:S_IFREG)|0777;
  st->st_size=ni->data_size;
  st->st_blocks=(ni->allocated_size+511)/512;
  st->st_blksize=v->volume->cluster_size;
  st->st_atim=ntfs2timespec(ni->last_access_time);
  st->st_mtim=ntfs2timespec(ni->last_data_change_time);
  st->st_ctim=ntfs2timespec(ni->last_mft_change_time);
}
int usbntfs_stat(const char *path,struct stat *st) {
  pthread_mutex_lock(&lock);
  const char *rel; usb_volume_t *v=get_volume(path,&rel);
  ntfs_inode *ni=v?ntfs_pathname_to_inode(v->volume,NULL,rel):NULL;
  int result=-1;
  if(ni) { inode_stat(v,ni,st); result=ntfs_inode_close(ni); }
  pthread_mutex_unlock(&lock); return result;
}
int usbntfs_statvfs(const char *path,struct statvfs *st) {
  pthread_mutex_lock(&lock);
  const char *rel; usb_volume_t *v=get_volume(path,&rel);
  int result=-1;
  if(v && (NVolFreeSpaceKnown(v->volume) || !ntfs_volume_get_free_space(v->volume))) {
    memset(st,0,sizeof(*st)); st->f_bsize=st->f_frsize=v->volume->cluster_size;
    st->f_blocks=v->volume->nr_clusters; st->f_bfree=st->f_bavail=v->volume->free_clusters;
    st->f_namemax=255; result=0;
    remember_capacity(v);
  }
  pthread_mutex_unlock(&lock); return result;
}
void usbntfs_spaces(strbuf_t *b,const char *current,int *first,int rescan) {
  pthread_mutex_lock(&lock);
  scan_locked(rescan);
  for(size_t i=0;i<volume_count;i++) {
    usb_volume_t *v=&volumes[i]; if(!v->present)continue;
    if(!v->format_error && !v->volume && (!v->write_checked || rescan)) {
      /* Listing storage must not open an unvisited partition for writing. */
      ntfs_volume *probe=inspect_volume(v);
      if(probe)ntfs_umount(probe,FALSE);
    }
    const char *read_only_code=v->format_error?NULL:v->read_only_code;
    remember_capacity(v);
    if(!*first)strbuf_append(b,",");
    *first=0;
    strbuf_append(b,"{\"path\":"); json_escape(b,v->root);
    strbuf_printf(b,",\"label_key\":\"storageNtfs\",\"label\":\"%s\",\"available\":%s,\"free\":%llu,\"total\":%llu,\"current\":%s",v->root+9,
      v->space_known?"true":"null",(unsigned long long)v->free_bytes,(unsigned long long)v->total_bytes,
      current && !strncmp(current,v->root,strlen(v->root)) &&
      (!current[strlen(v->root)] || current[strlen(v->root)]=='/')?"true":"false");
    if(read_only_code) {
      strbuf_append(b,",\"read_only\":true,\"read_only_code\":");
      json_escape(b,read_only_code);
    }
    strbuf_append(b,"}");
  }
  pthread_mutex_unlock(&lock);
}
static int fill_directory(void *context,const ntfschar *name,int len,int kind,s64 position,MFT_REF ref,unsigned type) {
  (void)position;
  usbntfs_dir_t *dir=context;
  if(kind==FILE_NAME_DOS || MREF(ref)<FILE_first_user)return 0;
  char *utf8=NULL;
  if(ntfs_ucstombs(name,len,&utf8,0)<0)return -1;
  if(strlen(utf8)>=sizeof(((struct dirent *)0)->d_name)) { free(utf8); errno=ENAMETOOLONG; return -1; }
  if(dir->count==dir->capacity) {
    size_t capacity=dir->capacity?dir->capacity*2:32;
    usb_entry_t *entries=realloc(dir->entries,capacity*sizeof(*entries));
    if(!entries) { free(utf8); return -1; }
    dir->entries=entries; dir->capacity=capacity;
  }
  dir->entries[dir->count++]=(usb_entry_t){utf8,MREF(ref),type==NTFS_DT_DIR?DT_DIR:DT_REG};
  return 0;
}
usbntfs_dir_t *usbntfs_opendir(const char *path) {
  pthread_mutex_lock(&lock);
  const char *rel; usb_volume_t *v=get_volume(path,&rel);
  ntfs_inode *ni=v?lookup(v,rel,0):NULL;
  usbntfs_dir_t *dir=NULL;
  if(ni) {
    if(!(ni->mrec->flags&MFT_RECORD_IS_DIRECTORY))errno=ENOTDIR;
    else {
      dir=calloc(1,sizeof(*dir));
      if(dir) {
        s64 position=0;
        if(ntfs_readdir(ni,&position,dir,fill_directory)) {
          int error=errno; usbntfs_closedir(dir); dir=NULL; errno=error;
        }
      }
    }
    int error=errno; ntfs_inode_close(ni); errno=error;
  }
  pthread_mutex_unlock(&lock); return dir;
}
struct dirent *usbntfs_readdir(usbntfs_dir_t *dir) {
  if(dir->index==dir->count)return NULL;
  usb_entry_t *entry=&dir->entries[dir->index++];
  memset(&dir->entry,0,sizeof(dir->entry));
  strcpy(dir->entry.d_name,entry->name);
  dir->entry.d_ino=entry->inode; dir->entry.d_type=entry->type;
  return &dir->entry;
}
int usbntfs_dir_error(usbntfs_dir_t *dir) { (void)dir; return 0; }
int usbntfs_closedir(usbntfs_dir_t *dir) {
  for(size_t i=0;i<dir->count;i++)free(dir->entries[i].name);
  free(dir->entries); free(dir); return 0;
}

/* The caller holds the library lock. Metadata inodes must never be mutated. */
static ntfs_inode *parent_inode(usb_volume_t *v,const char *rel,ntfschar **name,int *length) {
  if(require_writable(v))return NULL;
  char parent[PATH_MAX];
  if(path_dirname(rel,parent,sizeof(parent))) { errno=ENAMETOOLONG; return NULL; }
  *length=ntfs_mbstoucs(path_basename(rel),name);
  if(*length<=0 || *length>255) { errno=EINVAL; return NULL; }
  if(ntfs_forbidden_names(v->volume,*name,*length,TRUE)) { errno=EINVAL; return NULL; }
  ntfs_inode *ni=lookup(v,parent,0);
  if(ni && ni->mft_no==FILE_Extend) { ntfs_inode_close(ni); errno=EPERM; return NULL; }
  return ni;
}
static ntfs_inode *create_inode(usb_volume_t *v,const char *rel,int directory) {
  ntfschar *name=NULL; int len;
  ntfs_inode *parent=parent_inode(v,rel,&name,&len),*ni=NULL;
  if(parent) {
    ni=ntfs_create(parent,const_cpu_to_le32(0),name,(u8)len,directory?S_IFDIR:S_IFREG);
    int error=errno; if(ntfs_inode_close(parent) && ni) { ntfs_inode_close(ni); ni=NULL; error=errno; } errno=error;
  }
  free(name); return ni;
}
static int inode_busy(usb_volume_t *volume,uint64_t inode);
int usbntfs_open(const char *path,int flags) {
  pthread_mutex_lock(&lock);
  size_t i; for(i=0;i<NTFS_FILES && files[i].inode;i++);
  int result=-1;
  if(i==NTFS_FILES) { errno=EMFILE; goto done; }
  const char *rel; usb_volume_t *v=get_volume(path,&rel);
  if(!v)goto done;
  int writing=(flags&O_ACCMODE)!=O_RDONLY || (flags&(O_CREAT|O_TRUNC));
  ntfs_inode *ni=lookup(v,rel,writing);
  if(ni && (flags&O_CREAT) && (flags&O_EXCL)) { ntfs_inode_close(ni); errno=EEXIST; goto done; }
  if(!ni && errno==ENOENT && (flags&O_CREAT))ni=create_inode(v,rel,0);
  if(!ni)goto done;
  if(writing && inode_busy(v,ni->mft_no)) { ntfs_inode_close(ni); errno=EBUSY; goto done; }
  if(ni->mrec->flags&MFT_RECORD_IS_DIRECTORY) { ntfs_inode_close(ni); errno=EISDIR; goto done; }
  ntfs_attr *data=ntfs_attr_open(ni,AT_DATA,AT_UNNAMED,0);
  if(!data || ((flags&O_TRUNC) && ntfs_attr_truncate(data,0))) {
    int error=errno; if(data)ntfs_attr_close(data); ntfs_inode_close(ni); errno=error; goto done;
  }
  files[i]=(usb_file_t){v,ni,data,(flags&O_APPEND)?data->data_size:0,flags};
  v->refs++; result=NTFS_FD_BASE+(int)i;
done:
  pthread_mutex_unlock(&lock); return result;
}
static usb_file_t *file_for(int fd) {
  if(!usbntfs_fd(fd) || !files[fd-NTFS_FD_BASE].inode) { errno=EBADF; return NULL; }
  return &files[fd-NTFS_FD_BASE];
}
static int inode_busy(usb_volume_t *volume,uint64_t inode) {
  for(size_t i=0;i<NTFS_FILES;i++)
    if(files[i].inode && files[i].volume==volume && files[i].inode->mft_no==inode) { errno=EBUSY; return 1; }
  return 0;
}
int usbntfs_close(int fd) {
  pthread_mutex_lock(&lock);
  usb_file_t *f=file_for(fd); int result=-1;
  if(f) {
    ntfs_attr_close(f->data); result=ntfs_inode_close(f->inode);
    if(!result)result=dev_sync(f->volume->volume->dev);
    f->volume->used=time(NULL);
    f->volume->refs--; memset(f,0,sizeof(*f));
  }
  pthread_mutex_unlock(&lock); return result;
}
int usbntfs_fstat(int fd,struct stat *st) {
  pthread_mutex_lock(&lock);
  usb_file_t *f=file_for(fd); int result=-1;
  if(f) { inode_stat(f->volume,f->inode,st); st->st_size=f->data->data_size; result=0; }
  pthread_mutex_unlock(&lock); return result;
}
static ssize_t file_io(int fd,void *data,size_t size,off_t offset,int writing) {
  pthread_mutex_lock(&lock);
  usb_file_t *f=file_for(fd); ssize_t result=-1;
  if(f) {
    if((writing && (f->flags&O_ACCMODE)==O_RDONLY) || (!writing && (f->flags&O_ACCMODE)==O_WRONLY))errno=EBADF;
    else {
      off_t at=offset<0?f->position:offset;
      if(writing && (f->flags&O_APPEND))at=f->data->data_size;
      result=writing?ntfs_attr_pwrite(f->data,at,(s64)size,data):ntfs_attr_pread(f->data,at,(s64)size,data);
      if(result>0 && offset<0)f->position=at+result;
      if(result>0 && writing)ntfs_inode_update_times(f->inode,NTFS_UPDATE_MCTIME);
    }
  }
  pthread_mutex_unlock(&lock); return result;
}
ssize_t usbntfs_read(int fd,void *data,size_t size) { return file_io(fd,data,size,-1,0); }
ssize_t usbntfs_pread(int fd,void *data,size_t size,off_t offset) {
  if(offset<0) { errno=EINVAL; return -1; }
  return file_io(fd,data,size,offset,0);
}
ssize_t usbntfs_write(int fd,const void *data,size_t size) { return file_io(fd,(void *)data,size,-1,1); }
off_t usbntfs_seek(int fd,off_t offset,int whence) {
  pthread_mutex_lock(&lock);
  usb_file_t *f=file_for(fd); off_t result=-1;
  if(f) {
    off_t base=whence==SEEK_SET?0:whence==SEEK_CUR?f->position:whence==SEEK_END?f->data->data_size:-1;
    if(base<0 || offset < -base || (offset>0 && base>INT64_MAX-offset))errno=EINVAL;
    else result=f->position=base+offset;
  }
  pthread_mutex_unlock(&lock); return result;
}
int usbntfs_sync(int fd) {
  pthread_mutex_lock(&lock);
  usb_file_t *f=file_for(fd);
  int result=f?ntfs_inode_sync(f->inode):-1;
  if(!result)result=dev_sync(f->volume->volume->dev);
  pthread_mutex_unlock(&lock); return result;
}
int usbntfs_mkdir(const char *path) {
  pthread_mutex_lock(&lock);
  const char *rel; usb_volume_t *v=get_volume(path,&rel);
  ntfs_inode *ni=v?create_inode(v,rel,1):NULL;
  int result=ni?ntfs_inode_close(ni):-1;
  pthread_mutex_unlock(&lock); return result;
}
static int remove_inode(usb_volume_t *v,const char *rel,int directory) {
  ntfs_inode *ni=lookup(v,rel,1); if(!ni)return -1;
  if(inode_busy(v,ni->mft_no)) { ntfs_inode_close(ni); errno=EBUSY; return -1; }
  int is_dir=!!(ni->mrec->flags&MFT_RECORD_IS_DIRECTORY);
  if(directory!=is_dir || (is_dir && le16_to_cpu(ni->mrec->link_count)==1 && ntfs_check_empty_dir(ni))) {
    int error=directory!=is_dir?(is_dir?EISDIR:ENOTDIR):errno;
    ntfs_inode_close(ni); errno=error; return -1;
  }
  ntfschar *name=NULL; int len;
  ntfs_inode *parent=parent_inode(v,rel,&name,&len);
  if(!parent) { ntfs_inode_close(ni); free(name); return -1; }
  int result=ntfs_delete(v->volume,rel,ni,parent,name,(u8)len);
  free(name); return result;
}
int usbntfs_remove(const char *path,int directory) {
  pthread_mutex_lock(&lock);
  const char *rel; usb_volume_t *v=get_volume(path,&rel);
  int result=v?remove_inode(v,rel,directory):-1;
  pthread_mutex_unlock(&lock); return result;
}
static int link_inode(usb_volume_t *v,const char *source,const char *target) {
  ntfs_inode *ni=lookup(v,source,1); if(!ni)return -1;
  ntfschar *name=NULL; int len;
  ntfs_inode *parent=parent_inode(v,target,&name,&len);
  int result=parent?ntfs_link(ni,parent,name,(u8)len):-1;
  int error=errno;
  if(parent && ntfs_inode_close(parent) && !result) { result=-1; error=errno; }
  if(ntfs_inode_close(ni) && !result) { result=-1; error=errno; }
  free(name); errno=error; return result;
}
int usbntfs_rename(const char *source,const char *target) {
  pthread_mutex_lock(&lock);
  const char *src,*dst; usb_volume_t *a=get_volume(source,&src),*b=NULL;
  int result=-1;
  if(a) { a->refs++; b=get_volume(target,&dst); }
  if(!a || !b)goto done;
  if(a!=b) { errno=EXDEV; goto done; }
  ntfs_inode *ni=lookup(a,src,1); if(!ni)goto done;
  if(inode_busy(a,ni->mft_no)) { ntfs_inode_close(ni); errno=EBUSY; goto done; }
  int directory=!!(ni->mrec->flags&MFT_RECORD_IS_DIRECTORY);
  uint64_t inode=ni->mft_no; ntfs_inode_close(ni);
  if(directory && !strncmp(dst,src,strlen(src)) && dst[strlen(src)]=='/') { errno=EINVAL; goto done; }
  ni=ntfs_pathname_to_inode(a->volume,NULL,dst);
  char backup[PATH_MAX]={0};
  if(ni) {
    if(inode_busy(a,ni->mft_no)) { ntfs_inode_close(ni); errno=EBUSY; goto done; }
    int target_dir=!!(ni->mrec->flags&MFT_RECORD_IS_DIRECTORY);
    if(ni->mft_no==inode) { ntfs_inode_close(ni); result=0; goto done; }
    if(directory!=target_dir || (target_dir && ntfs_check_empty_dir(ni))) {
      int error=directory!=target_dir?(target_dir?EISDIR:ENOTDIR):errno;
      ntfs_inode_close(ni); errno=error; goto done;
    }
    ntfs_inode_close(ni);
    static unsigned sequence;
    int n=snprintf(backup,sizeof(backup),"%s.wfm-ntfs-%u",dst,++sequence);
    if(n<0 || (size_t)n>=sizeof(backup)) { errno=ENAMETOOLONG; goto done; }
    ni=ntfs_pathname_to_inode(a->volume,NULL,backup);
    if(ni) { ntfs_inode_close(ni); errno=EEXIST; goto done; }
    if(errno!=ENOENT || link_inode(a,dst,backup))goto done;
    if(remove_inode(a,dst,directory))goto done;
  } else if(errno!=ENOENT)goto done;
  /* Same link/unlink/rollback strategy as upstream ntfs_fuse_safe_rename. */
  if(link_inode(a,src,dst))goto restore;
  if(remove_inode(a,src,directory)) {
    int error=errno; remove_inode(a,dst,directory); errno=error; goto restore;
  }
  result=0;
  if(backup[0] && remove_inode(a,backup,directory))result=-1;
  goto done;
restore: {
    int error=errno;
    if(backup[0] && !link_inode(a,backup,dst))remove_inode(a,backup,directory);
    errno=error;
  }
done:
  if(a)a->refs--;
  pthread_mutex_unlock(&lock); return result;
}
int usbntfs_access(const char *path,int mode) {
  struct stat st;
  if(usbntfs_stat(path,&st))return -1;
  if(mode&W_OK) {
    pthread_mutex_lock(&lock);
    const char *rel; usb_volume_t *v=get_volume(path,&rel);
    int result=v?require_writable(v):-1;
    pthread_mutex_unlock(&lock); return result;
  }
  return 0;
}
void usbntfs_shutdown(void) {
  pthread_mutex_lock(&lock);
  for(size_t i=0;i<NTFS_FILES;i++) {
    usb_file_t *f=&files[i];
    if(!f->inode)continue;
    ntfs_attr_close(f->data); ntfs_inode_close(f->inode); f->volume->refs--;
    memset(f,0,sizeof(*f));
  }
  for(size_t i=0;i<volume_count;i++) {
    usb_volume_t *v=&volumes[i];
    if(!v->refs)close_volume(v);
  }
  pthread_mutex_unlock(&lock);
}
