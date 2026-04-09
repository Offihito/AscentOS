// ── I/O Syscalls: read, write, close, open, lseek ────────────────────────────
#include "syscall.h"
#include "console/klog.h"
#include "sched/sched.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "lib/string.h"
#include "../fb/framebuffer.h"
#include "../font/font.h"
#include "../console/console.h"
#include <stdint.h>

typedef struct {
  uint8_t *data;
  uint32_t capacity;
} ramfs_file_t;

#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGWINSZ 0x5413

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400

#define NCCS 32

#define F_GETFL 3
#define F_SETFL 4

// ── Linux x86_64 stat structure (matches musl struct stat) ────────────────────
struct kstat {
  uint64_t st_dev;      // Device
  uint64_t st_ino;      // Inode
  uint64_t st_nlink;    // Number of hard links

  uint32_t st_mode;     // Mode (file type + permissions)
  uint32_t st_uid;     // User ID
  uint32_t st_gid;     // Group ID
  uint32_t __pad0;     // Padding
  uint64_t st_rdev;     // Device ID (if special file)
  int64_t  st_size;     // Total size in bytes
  int64_t  st_blksize;  // Block size for filesystem I/O
  int64_t  st_blocks;   // Number of 512B blocks allocated

  int64_t  st_atim_sec;   // Access time seconds
  int64_t  st_atim_nsec;  // Access time nanoseconds
  int64_t  st_mtim_sec;   // Modification time seconds
  int64_t  st_mtim_nsec;  // Modification time nanoseconds
  int64_t  st_ctim_sec;   // Status change time seconds
  int64_t  st_ctim_nsec;  // Status change time nanoseconds
  int64_t  __unused[3];   // Unused padding
};

typedef unsigned int  tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int  speed_t;

struct termios {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t     c_line;
  cc_t     c_cc[NCCS];
  speed_t  __c_ispeed;
  speed_t  __c_ospeed;
};

struct winsize {
  unsigned short ws_row;
  unsigned short ws_col;
  unsigned short ws_xpixel;
  unsigned short ws_ypixel;
};

static struct termios console_termios;

static int alloc_fd(struct thread *t) {
  for (int i = 0; i < MAX_FDS; i++) {
    if (t->fds[i] == NULL)
      return i;
  }
  return -1; // ENFILE
}

static uint64_t sys_open(uint64_t path_ptr, uint64_t flags, uint64_t mode,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  const char *path = (const char *)path_ptr;
  if (!path) return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  if (!t) return (uint64_t)-1;

  int fd = alloc_fd(t);
  if (fd < 0) return (uint64_t)-24; // EMFILE

  vfs_node_t *node = vfs_resolve_path(path);
  if (!node) {
    if (flags & O_CREAT) {
      char parent_path[128];
      char file_name[128];
      size_t len = strlen(path);
      if (len == 0 || len >= sizeof(file_name))
        return (uint64_t)-14;

      const char *slash = 0;
      for (const char *p = path; *p; p++)
        if (*p == '/') slash = p;

      vfs_node_t *parent = fs_root;
      if (slash) {
        size_t parent_len = (size_t)(slash - path);
        if (parent_len == 0) {
          parent = fs_root;
        } else {
          if (parent_len >= sizeof(parent_path))
            return (uint64_t)-14;
          for (size_t i = 0; i < parent_len; i++)
            parent_path[i] = path[i];
          parent_path[parent_len] = '\0';
          parent = vfs_resolve_path(parent_path);
        }
        size_t file_len = strlen(slash + 1);
        if (file_len >= sizeof(file_name)) return (uint64_t)-14;
        strcpy(file_name, slash + 1);
      } else {
        size_t file_len = len;
        if (file_len >= sizeof(file_name)) return (uint64_t)-14;
        strcpy(file_name, path);
      }

      if (!parent || (parent->flags & 0x07) != FS_DIRECTORY)
        return (uint64_t)-20; // ENOTDIR

      if (vfs_create(parent, file_name, (uint16_t)mode) != 0)
        return (uint64_t)-17; // EEXIST or generic error

      node = vfs_finddir(parent, file_name);
      if (!node) return (uint64_t)-2; // ENOENT
    } else {
      klog_puts("[SYSCALL] sys_open failed to find: ");
      klog_puts(path);
      klog_puts("\n");
      return (uint64_t)-2; // ENOENT
    }
  }

  if ((flags & O_TRUNC) && node->flags == FS_FILE)
    node->length = 0;

  vfs_open(node);
  t->fds[fd] = node;
  t->fd_offsets[fd] = 0;

  return fd;
}

static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  uint32_t bytes_read = vfs_read(node, t->fd_offsets[fd], count, (uint8_t *)buf);
  t->fd_offsets[fd] += bytes_read;

  return bytes_read;
}

static uint64_t sys_ftruncate(uint64_t fd, uint64_t length, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  if (!node || node->flags != FS_FILE) return (uint64_t)-1; // EPERM
  if (!node->device) return (uint64_t)-1;

  ramfs_file_t *file = (ramfs_file_t *)node->device;
  uint32_t new_len = (uint32_t)length;
  if (new_len == 0) {
    if (file->data) {
      kfree(file->data);
      file->data = NULL;
      file->capacity = 0;
    }
    node->length = 0;
    return 0;
  }

  if (new_len > file->capacity) {
    uint32_t new_cap  = new_len;
    uint8_t *new_data = kmalloc(new_cap);
    if (!new_data) return (uint64_t)-12; // ENOMEM
    if (file->data) {
      memcpy(new_data, file->data, node->length);
      kfree(file->data);
    }
    if (new_len > node->length)
      memset(new_data + node->length, 0, new_len - node->length);
    file->data     = new_data;
    file->capacity = new_cap;
  } else if (new_len > node->length) {
    memset((uint8_t *)file->data + node->length, 0, new_len - node->length);
  }

  node->length = new_len;
  return 0;
}

// ── fd_write ─────────────────────────────────────────────────────────────────
// For fd 0/1/2 (stdout / stderr) we use console_write_batch() so that each
// write() syscall results in exactly ONE framebuffer blit.  This kills the
// per-character flicker that kilo was triggering.
static int64_t fd_write(int fd, const void *buf, size_t count) {
  if (fd == 1 || fd == 2) {
    // Batch the whole buffer, swap framebuffer once at the end.
    console_write_batch((const char *)buf, count);
    return (int64_t)count;
  }

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return -9; // EBADF

  vfs_node_t *node = t->fds[fd];
  uint32_t bytes_written =
      vfs_write(node, t->fd_offsets[fd], count, (uint8_t *)buf);
  t->fd_offsets[fd] += bytes_written;
  return (int64_t)bytes_written;
}

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  int64_t r = fd_write((int)fd, (const void *)buf, (size_t)count);
  return (uint64_t)r;
}

struct user_iovec {
  uint64_t iov_base;
  uint64_t iov_len;
};

static uint64_t sys_writev(uint64_t fd, uint64_t iov_u, uint64_t iovcnt,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  if (iovcnt == 0)   return 0;
  if (iovcnt > 1024) return (uint64_t)-22; // EINVAL

  struct user_iovec *iov = (struct user_iovec *)iov_u;
  size_t total = 0;

  for (uint64_t i = 0; i < iovcnt; i++) {
    uint64_t base = iov[i].iov_base;
    uint64_t len  = iov[i].iov_len;
    if (len == 0) continue;
    int64_t w = fd_write((int)fd, (const void *)base, (size_t)len);
    if (w < 0)    return (uint64_t)w;
    total += (size_t)w;
    if ((size_t)w != len) break;
  }
  return total;
}

static uint64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS) return (uint64_t)-9; // EBADF

  bool is_console_fd = (fd <= 2);
  if (!is_console_fd) {
    if (!t->fds[fd]) return (uint64_t)-9;
    if (t->fds[fd]->flags != FS_CHARDEV) return (uint64_t)-25; // ENOTTY
  }

  switch (request) {
  case TIOCGWINSZ: {
    struct winsize *ws = (struct winsize *)arg;
    if (!ws) return (uint64_t)-14; // EFAULT
    ws->ws_row    = (unsigned short)(fb_get_height() / FONT_HEIGHT);
    ws->ws_col    = (unsigned short)(fb_get_width()  / FONT_WIDTH);
    ws->ws_xpixel = (unsigned short)fb_get_width();
    ws->ws_ypixel = (unsigned short)fb_get_height();
    return 0;
  }
  case TCGETS: {
    struct termios *term = (struct termios *)arg;
    if (!term) return (uint64_t)-14;
    *term = console_termios;
    return 0;
  }
  case TCSETS:
  case TCSETSW:
  case TCSETSF: {
    const struct termios *term = (const struct termios *)arg;
    if (!term) return (uint64_t)-14;
    console_termios = *term;
    return 0;
  }
  default:
    return (uint64_t)-25; // ENOTTY
  }
}

static uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];

  int64_t new_offset = 0;
  if (whence == 0) {
    new_offset = (int64_t)offset;
  } else if (whence == 1) {
    new_offset = (int64_t)t->fd_offsets[fd] + (int64_t)offset;
  } else if (whence == 2) {
    new_offset = (int64_t)node->length + (int64_t)offset;
  } else {
    return (uint64_t)-22; // EINVAL
  }

  if (new_offset < 0) return (uint64_t)-22;

  t->fd_offsets[fd] = (uint32_t)new_offset;
  return (uint64_t)new_offset;
}

static uint64_t sys_close(uint64_t fd, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9; // EBADF

  vfs_close(t->fds[fd]);
  t->fds[fd]        = NULL;
  t->fd_offsets[fd] = 0;

  return 0;
}

static uint64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  (void)arg;

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9; // EBADF

  switch (cmd) {
  case F_GETFL:
    // Minimal compatibility: descriptors are treated as read/write.
    return O_RDWR;
  case F_SETFL:
    // Accept common status-flag updates (non-blocking/append/etc.) as no-op.
    return 0;
  default:
    return (uint64_t)-22; // EINVAL
  }
}

// ── Fill kstat from vfs_node ────────────────────────────────────────────────
static void fill_kstat(struct kstat *ks, vfs_node_t *node) {
  ks->st_dev     = 0;  // No device numbers yet
  ks->st_ino     = (uint64_t)node->inode;
  ks->st_nlink   = 1;  // No hard link tracking yet

  // Convert vfs flags to mode
  uint32_t mode = node->mask & 0777;  // Permission bits
  switch (node->flags & 0xFF) {
    case FS_FILE:      mode |= 0100000; break; // S_IFREG
    case FS_DIRECTORY: mode |= 0040000; break; // S_IFDIR
    case FS_CHARDEV:   mode |= 0020000; break; // S_IFCHR
    case FS_BLOCKDEV:  mode |= 0060000; break; // S_IFBLK
    case FS_SYMLINK:   mode |= 0120000; break; // S_IFLNK
    default:           mode |= 0100000; break; // Default to regular file
  }
  ks->st_mode    = mode;
  ks->st_uid     = node->uid;
  ks->st_gid     = node->gid;
  ks->__pad0     = 0;
  ks->st_rdev    = 0;
  ks->st_size    = (int64_t)node->length;
  ks->st_blksize = 4096;  // Reasonable default
  ks->st_blocks  = ((int64_t)node->length + 511) / 512;

  ks->st_atim_sec  = (int64_t)node->atime;
  ks->st_atim_nsec = 0;
  ks->st_mtim_sec  = (int64_t)node->mtime;
  ks->st_mtim_nsec = 0;
  ks->st_ctim_sec  = (int64_t)node->ctime;
  ks->st_ctim_nsec = 0;
  ks->__unused[0] = 0;
  ks->__unused[1] = 0;
  ks->__unused[2] = 0;
}

// ── sys_stat: stat(path, statbuf) ────────────────────────────────────────────
static uint64_t sys_stat(uint64_t path_ptr, uint64_t statbuf_ptr,
                         uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  const char *path = (const char *)path_ptr;
  struct kstat *ks = (struct kstat *)statbuf_ptr;

  if (!path || !ks) return (uint64_t)-14; // EFAULT

  vfs_node_t *node = vfs_resolve_path(path);
  if (!node) {
    klog_puts("[SYSCALL] sys_stat: not found: ");
    klog_puts(path);
    klog_puts("\n");
    return (uint64_t)-2; // ENOENT
  }

  fill_kstat(ks, node);
  return 0;
}

// ── sys_fstat: fstat(fd, statbuf) ─────────────────────────────────────────────
static uint64_t sys_fstat(uint64_t fd, uint64_t statbuf_ptr,
                          uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  struct kstat *ks = (struct kstat *)statbuf_ptr;

  if (!ks) return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  fill_kstat(ks, node);
  return 0;
}

// Linux getdents64 structure (binary compatible with musl/glibc)
struct linux_dirent64 {
  uint64_t d_ino;     // Inode number
  uint64_t d_off;     // Offset to next entry
  uint16_t d_reclen;  // Size of this dirent
  uint8_t  d_type;    // File type (DT_REG, DT_DIR, etc.)
  char     d_name[];  // Filename (null-terminated)
};

// DT_* constants from Linux dirent.h
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK     10
#define DT_SOCK    12

// getdents64(fd, dirp, count) - read directory entries
static uint64_t sys_getdents64(uint64_t fd, uint64_t dirp, uint64_t count,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  if ((node->flags & 0xFF) != FS_DIRECTORY) return (uint64_t)-20; // ENOTDIR

  uint8_t *buf = (uint8_t *)dirp;
  if (!buf) return (uint64_t)-14; // EFAULT

  size_t written = 0;
  uint32_t index = t->fd_offsets[fd]; // Start from saved position

  while (1) {
    struct dirent *de = vfs_readdir(node, index);
    if (!de) break; // No more entries

    size_t name_len = strlen(de->name);
    size_t entry_size = sizeof(struct linux_dirent64) + name_len + 1;
    entry_size = (entry_size + 7) & ~7; // Align to 8 bytes

    if (written + entry_size > count) {
      // Buffer full - return what we have, don't advance index
      break;
    }

    struct linux_dirent64 *entry = (struct linux_dirent64 *)(buf + written);
    entry->d_ino = de->ino;
    entry->d_off = (uint64_t)(index + 1);
    entry->d_reclen = (uint16_t)entry_size;

    // Determine file type from VFS node
    vfs_node_t *child = vfs_finddir(node, de->name);
    if (child) {
      switch (child->flags & 0xFF) {
        case FS_FILE:      entry->d_type = DT_REG; break;
        case FS_DIRECTORY: entry->d_type = DT_DIR; break;
        case FS_CHARDEV:   entry->d_type = DT_CHR; break;
        case FS_BLOCKDEV:  entry->d_type = DT_BLK; break;
        case FS_SYMLINK:   entry->d_type = DT_LNK; break;
        default:          entry->d_type = DT_UNKNOWN; break;
      }
    } else {
      entry->d_type = DT_UNKNOWN;
    }

    strcpy(entry->d_name, de->name);
    written += entry_size;
    index++;
  }

  // Update saved position for next call
  t->fd_offsets[fd] = index;

  return written;
}

void syscall_register_io(void) {
  syscall_register(SYS_READ,       sys_read);
  syscall_register(SYS_WRITE,      sys_write);
  syscall_register(SYS_WRITEV,     sys_writev);
  syscall_register(SYS_IOCTL,      sys_ioctl);
  syscall_register(SYS_OPEN,       sys_open);
  syscall_register(SYS_CLOSE,      sys_close);
  syscall_register(SYS_LSEEK,      sys_lseek);
  syscall_register(SYS_FTRUNCATE,  sys_ftruncate);
  syscall_register(SYS_FCNTL,      sys_fcntl);
  syscall_register(SYS_STAT,       sys_stat);
  syscall_register(SYS_FSTAT,      sys_fstat);
  syscall_register(SYS_GETDENTS64, sys_getdents64);
}