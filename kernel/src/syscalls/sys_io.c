// ── I/O Syscalls: read, write, close, open, lseek ────────────────────────────
#include "../console/console.h"
#include "../console/klog.h"
#include "../drivers/audio/sb16.h"
#include "../drivers/timer/pit.h"
#include "../fb/framebuffer.h"
#include "../font/font.h"
#include "../fs/ramfs.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../sched/sched.h"
#include "../net/net.h"
#include "syscall.h"
#include <stdint.h>

// User-space pointer validation: reject kernel/HHDM addresses
#define USER_ADDR_MAX 0x00007FFFFFFFFFFFULL
static inline bool is_user_ptr(uint64_t addr) {
  return addr != 0 && addr <= USER_ADDR_MAX;
}

typedef struct {
  uint8_t *data;
  uint32_t capacity;
} ramfs_file_t;

#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TCSETSF 0x5404
#define TIOCGWINSZ 0x5413

// OSS /dev/dsp ioctls
#define SNDCTL_DSP_RESET 0x00005000
#define SNDCTL_DSP_SPEED 0xC0045002
#define SNDCTL_DSP_STEREO 0xC0045003
#define SNDCTL_DSP_SETFMT 0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006

#define AFMT_U8 0x00000008
#define AFMT_S16_LE 0x00000010

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x40
#define O_TRUNC 0x200
#define O_APPEND 0x400

#define F_GETFL 3
#define F_SETFL 4

#define AT_FDCWD -100

// ── Linux x86_64 stat structure (matches musl struct stat)
// ────────────────────
struct kstat {
  uint64_t st_dev;   // Device
  uint64_t st_ino;   // Inode
  uint64_t st_nlink; // Number of hard links

  uint32_t st_mode;   // Mode (file type + permissions)
  uint32_t st_uid;    // User ID
  uint32_t st_gid;    // Group ID
  uint32_t __pad0;    // Padding
  uint64_t st_rdev;   // Device ID (if special file)
  int64_t st_size;    // Total size in bytes
  int64_t st_blksize; // Block size for filesystem I/O
  int64_t st_blocks;  // Number of 512B blocks allocated

  int64_t st_atim_sec;  // Access time seconds
  int64_t st_atim_nsec; // Access time nanoseconds
  int64_t st_mtim_sec;  // Modification time seconds
  int64_t st_mtim_nsec; // Modification time nanoseconds
  int64_t st_ctim_sec;  // Status change time seconds
  int64_t st_ctim_nsec; // Status change time nanoseconds
  int64_t __unused[3];  // Unused padding
};

struct statx_timestamp {
  int64_t tv_sec;
  uint32_t tv_nsec;
  int32_t __reserved;
};

struct statx {
  uint32_t stx_mask;
  uint32_t stx_blksize;
  uint64_t stx_attributes;
  uint32_t stx_nlink;
  uint32_t stx_uid;
  uint32_t stx_gid;
  uint16_t stx_mode;
  uint16_t __spare0[1];
  uint64_t stx_ino;
  uint64_t stx_size;
  uint64_t stx_blocks;
  uint64_t stx_attributes_mask;
  struct statx_timestamp stx_atime;
  struct statx_timestamp stx_btime;
  struct statx_timestamp stx_ctime;
  struct statx_timestamp stx_mtime;
  uint32_t stx_rdev_major;
  uint32_t stx_rdev_minor;
  uint32_t stx_dev_major;
  uint32_t stx_dev_minor;
  uint64_t __spare2[14];
};

#define STATX_TYPE 0x0001U
#define STATX_MODE 0x0002U
#define STATX_NLINK 0x0004U
#define STATX_UID 0x0008U
#define STATX_GID 0x0010U
#define STATX_ATIME 0x0020U
#define STATX_MTIME 0x0040U
#define STATX_CTIME 0x0080U
#define STATX_INO 0x0100U
#define STATX_SIZE 0x0200U
#define STATX_BLOCKS 0x0400U
#define STATX_BASIC_STATS 0x07ffU
#define STATX_BTIME 0x0800U

#define AT_STATX_SYNC_AS_STAT 0x0000
#define AT_STATX_FORCE_SYNC 0x2000
#define AT_STATX_DONT_SYNC 0x4000
#define AT_STATX_SYNC_TYPE 0x6000
#define AT_EMPTY_PATH 0x1000

#include "../fb/terminal.h"

struct termios console_termios;

static int alloc_fd(struct thread *t) {
  for (int i = 0; i < MAX_FDS; i++) {
    if (t->fds[i] == NULL)
      return i;
  }
  return -1; // ENFILE
}

static uint64_t do_sys_open(int dirfd, const char *path, uint64_t flags,
                            uint64_t mode) {
  if (!path)
    return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  int fd = alloc_fd(t);
  if (fd < 0)
    return (uint64_t)-24; // EMFILE

  vfs_node_t *base_dir = fs_root;
  if (path[0] != '/') {
    if (dirfd == AT_FDCWD) {
      base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
      if (!base_dir)
        base_dir = fs_root; // Fallback
    } else {
      if (dirfd < 0 || dirfd >= MAX_FDS || !t->fds[dirfd])
        return (uint64_t)-9; // EBADF
      base_dir = t->fds[dirfd];
      if ((base_dir->flags & 0xFF) != FS_DIRECTORY)
        return (uint64_t)-20; // ENOTDIR
    }
  }

  // Check if this is a device file path (/dev/...)
  vfs_node_t *node = NULL;
  if (strncmp(path, "/dev/", 5) == 0) {
    // Try to get from device registry first
    node = fb_lookup_device((char *)path + 5); // Skip "/dev/"
  }

  // Fall back to normal VFS path resolution
  if (!node) {
    node = vfs_resolve_path_at(base_dir, path);
  }

  if (!node) {
    if (flags & O_CREAT) {
      char parent_path[128];
      char file_name[128];
      size_t len = strlen(path);
      if (len == 0 || len >= sizeof(file_name))
        return (uint64_t)-14;

      const char *slash = 0;
      for (const char *p = path; *p; p++)
        if (*p == '/')
          slash = p;

      vfs_node_t *parent = base_dir;
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
          parent = vfs_resolve_path_at(base_dir, parent_path);
        }
        size_t file_len = strlen(slash + 1);
        if (file_len >= sizeof(file_name))
          return (uint64_t)-14;
        strcpy(file_name, slash + 1);
      } else {
        size_t file_len = len;
        if (file_len >= sizeof(file_name))
          return (uint64_t)-14;
        strcpy(file_name, path);
      }

      if (!parent || (parent->flags & 0x07) != FS_DIRECTORY)
        return (uint64_t)-20; // ENOTDIR

      // Umask equivalent (simplified)
      mode = mode & ~0022; // Basic default umask

      if (vfs_create(parent, file_name, (uint16_t)mode) != 0)
        return (uint64_t)-17; // EEXIST or generic error

      node = vfs_finddir(parent, file_name);
      if (!node)
        return (uint64_t)-2; // ENOENT
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

static uint64_t sys_open(uint64_t path_ptr, uint64_t flags, uint64_t mode,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  return do_sys_open(AT_FDCWD, (const char *)path_ptr, flags, mode);
}

static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count, uint64_t a3,
                         uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (!is_user_ptr(buf))
    return (uint64_t)-14; // EFAULT
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  uint32_t bytes_read =
      vfs_read(node, t->fd_offsets[fd], count, (uint8_t *)buf);
  t->fd_offsets[fd] += bytes_read;

  return bytes_read;
}

static uint64_t sys_ftruncate(uint64_t fd, uint64_t length, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  if (!node || (node->flags & 0xFF) != FS_FILE)
    return (uint64_t)-1; // EPERM

  if (vfs_truncate(node, (uint32_t)length) == 0) {
    return 0;
  }
  return (uint64_t)-1;
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
  (void)a3;
  (void)a4;
  (void)a5;
  if (fd > 2 && !is_user_ptr(buf))
    return (uint64_t)-14; // EFAULT
  int64_t r = fd_write((int)fd, (const void *)buf, (size_t)count);
  return (uint64_t)r;
}

struct user_iovec {
  uint64_t iov_base;
  uint64_t iov_len;
};

static uint64_t sys_writev(uint64_t fd, uint64_t iov_u, uint64_t iovcnt,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (iovcnt == 0)
    return 0;
  if (iovcnt > 1024)
    return (uint64_t)-22; // EINVAL

  struct user_iovec *iov = (struct user_iovec *)iov_u;
  size_t total = 0;

  for (uint64_t i = 0; i < iovcnt; i++) {
    uint64_t base = iov[i].iov_base;
    uint64_t len = iov[i].iov_len;
    if (len == 0)
      continue;
    int64_t w = fd_write((int)fd, (const void *)base, (size_t)len);
    if (w < 0)
      return (uint64_t)w;
    total += (size_t)w;
    if ((size_t)w != len)
      break;
  }
  return total;
}

static uint64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  // Most ioctls take pointers. A few take ints. However, no valid integer
  // argument or user pointer should ever be in the kernel/HHDM address range.
  if (arg > USER_ADDR_MAX)
    return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS)
    return (uint64_t)-9; // EBADF

  bool is_console_fd = (fd <= 2);
  if (!is_console_fd) {
    if (!t->fds[fd])
      return (uint64_t)-9;
    if ((t->fds[fd]->flags & 0xFF) != FS_CHARDEV)
      return (uint64_t)-25; // ENOTTY

    vfs_node_t *node = t->fds[fd];
    if (node->ioctl) {
      return (uint64_t)node->ioctl(node, (uint32_t)request, arg);
    }
  }

  switch ((uint32_t)request) {
  case TIOCGWINSZ: {
    struct winsize *ws = (struct winsize *)arg;
    if (!ws)
      return (uint64_t)-14; // EFAULT
    ws->ws_row = (unsigned short)(fb_get_height() / FONT_HEIGHT);
    ws->ws_col = (unsigned short)(fb_get_width() / FONT_WIDTH);
    ws->ws_xpixel = (unsigned short)fb_get_width();
    ws->ws_ypixel = (unsigned short)fb_get_height();
    return 0;
  }
  case TCGETS: {
    struct termios *term = (struct termios *)arg;
    if (!term)
      return (uint64_t)-14;
    *term = console_termios;
    return 0;
  }
  case TCSETS:
  case TCSETSW:
  case TCSETSF: {
    const struct termios *term = (const struct termios *)arg;
    if (!term)
      return (uint64_t)-14;
    console_termios = *term;
    return 0;
  }
  case 0x5470: { // KBDSCANMODE_GET
    int *mode = (int *)arg;
    if (!mode)
      return (uint64_t)-14;
    extern bool keyboard_is_scancode_mode(void);
    *mode = keyboard_is_scancode_mode() ? 1 : 0;
    return 0;
  }
  case 0x5471: { // KBDSCANMODE_SET
    int mode = (int)arg;
    extern void keyboard_set_scancode_mode(bool enabled);
    keyboard_set_scancode_mode(mode != 0);
    return 0;
  }
  case 0x5472: { // KBDSCANCODE_READ
    // Scancode event structure: {scancode(1), is_extended(1), is_release(1)}
    unsigned char *event = (unsigned char *)arg;
    if (!event)
      return (uint64_t)-14;

    extern bool keyboard_has_scancode(void);
    extern bool keyboard_get_scancode(void *event_ptr);

    if (keyboard_has_scancode()) {
      if (keyboard_get_scancode((void *)event)) {
        return 1; // 1 scancode event read
      }
    }
    return 0; // No scancode available (would block in non-blocking mode)
  }
  default:
    return (uint64_t)-25; // ENOTTY
  }
}

static uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

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

  if (new_offset < 0)
    return (uint64_t)-22;

  t->fd_offsets[fd] = (uint32_t)new_offset;
  return (uint64_t)new_offset;
}

static uint64_t sys_close(uint64_t fd, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

  vfs_close(t->fds[fd]);
  t->fds[fd] = NULL;
  t->fd_offsets[fd] = 0;

  return 0;
}

static uint64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg, uint64_t a3,
                          uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  (void)arg;

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

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
  ks->st_dev = 0; // No device numbers yet
  ks->st_ino = (uint64_t)node->inode;
  ks->st_nlink = 1; // No hard link tracking yet

  // Convert vfs flags to mode
  uint32_t mode = node->mask & 0777; // Permission bits
  switch (node->flags & 0xFF) {
  case FS_FILE:
    mode |= 0100000;
    break; // S_IFREG
  case FS_DIRECTORY:
    mode |= 0040000;
    break; // S_IFDIR
  case FS_CHARDEV:
    mode |= 0020000;
    break; // S_IFCHR
  case FS_BLOCKDEV:
    mode |= 0060000;
    break; // S_IFBLK
  case FS_SYMLINK:
    mode |= 0120000;
    break; // S_IFLNK
  default:
    mode |= 0100000;
    break; // Default to regular file
  }
  ks->st_mode = mode;
  ks->st_uid = node->uid;
  ks->st_gid = node->gid;
  ks->__pad0 = 0;
  ks->st_rdev = 0;
  ks->st_size = (int64_t)node->length;
  ks->st_blksize = 4096; // Reasonable default
  ks->st_blocks = ((int64_t)node->length + 511) / 512;

  ks->st_atim_sec = (int64_t)node->atime;
  ks->st_atim_nsec = 0;
  ks->st_mtim_sec = (int64_t)node->mtime;
  ks->st_mtim_nsec = 0;
  ks->st_ctim_sec = (int64_t)node->ctime;
  ks->st_ctim_nsec = 0;
  ks->__unused[0] = 0;
  ks->__unused[1] = 0;
  ks->__unused[2] = 0;
}

// ── sys_stat: stat(path, statbuf) ────────────────────────────────────────────
static uint64_t sys_stat(uint64_t path_ptr, uint64_t statbuf_ptr, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)path_ptr;
  struct kstat *ks = (struct kstat *)statbuf_ptr;

  if (!path || !ks)
    return (uint64_t)-14; // EFAULT
  if (!is_user_ptr((uint64_t)ks))
    return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  vfs_node_t *cwd_node = fs_root;
  if (t && t->cwd_path[0]) {
    cwd_node = vfs_resolve_path_at(fs_root, t->cwd_path);
    if (!cwd_node)
      cwd_node = fs_root;
  }

  vfs_node_t *node = vfs_resolve_path_at(cwd_node, path);
  if (!node) {
    klog_puts("[SYSCALL] sys_stat: not found: ");
    klog_puts(path);
    klog_puts("\n");
    return (uint64_t)-2; // ENOENT
  }

  fill_kstat(ks, node);
  return 0;
}

// ── sys_fstat: fstat(fd, statbuf)
// ─────────────────────────────────────────────
static uint64_t sys_fstat(uint64_t fd, uint64_t statbuf_ptr, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct kstat *ks = (struct kstat *)statbuf_ptr;

  if (!ks)
    return (uint64_t)-14; // EFAULT
  if (!is_user_ptr((uint64_t)ks))
    return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  fill_kstat(ks, node);
  return 0;
}

// ── sys_statx: statx(dirfd, path, flags, mask, statxbuf) ─────────────────────
static uint64_t sys_statx(uint64_t dirfd, uint64_t path_ptr, uint64_t flags,
                          uint64_t mask, uint64_t statxbuf_ptr, uint64_t a5) {
  (void)a5;
  (void)mask;
  const char *path = (const char *)path_ptr;
  struct statx *stx = (struct statx *)statxbuf_ptr;

  if (!stx || !is_user_ptr((uint64_t)stx))
    return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  vfs_node_t *node = NULL;

  if (path && path[0] != '\0') {
    vfs_node_t *base_dir = fs_root;
    if (path[0] != '/') {
      if ((int)dirfd == AT_FDCWD) {
        base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
        if (!base_dir)
          base_dir = fs_root;
      } else {
        if ((int)dirfd < 0 || (int)dirfd >= MAX_FDS || !t->fds[dirfd])
          return (uint64_t)-9; // EBADF
        base_dir = t->fds[dirfd];
      }
    }
    node = vfs_resolve_path_at(base_dir, path);
  } else {
    // pathname is NULL or empty string
    if (flags & AT_EMPTY_PATH) {
      if ((int)dirfd == AT_FDCWD) {
        node = vfs_resolve_path_at(fs_root, t->cwd_path);
        if (!node)
          node = fs_root;
      } else {
        if ((int)dirfd < 0 || (int)dirfd >= MAX_FDS || !t->fds[dirfd])
          return (uint64_t)-9; // EBADF
        node = t->fds[dirfd];
      }
    } else {
      return (uint64_t)-14; // EFAULT
    }
  }

  if (!node)
    return (uint64_t)-2; // ENOENT

  // Zero the whole structure first
  memset(stx, 0, sizeof(struct statx));

  // Populate statx
  stx->stx_mask = STATX_BASIC_STATS; // We support most basic attributes
  stx->stx_blksize = 4096;
  stx->stx_nlink = 1;
  stx->stx_uid = node->uid;
  stx->stx_gid = node->gid;

  uint32_t mode = node->mask & 0777;
  switch (node->flags & 0xFF) {
  case FS_FILE:
    mode |= 0100000;
    break;
  case FS_DIRECTORY:
    mode |= 0040000;
    break;
  case FS_CHARDEV:
    mode |= 0020000;
    break;
  case FS_BLOCKDEV:
    mode |= 0060000;
    break;
  case FS_SYMLINK:
    mode |= 0120000;
    break;
  default:
    mode |= 0100000;
    break;
  }
  stx->stx_mode = (uint16_t)mode;
  stx->stx_ino = node->inode;
  stx->stx_size = node->length;
  stx->stx_blocks = (node->length + 511) / 512;

  stx->stx_atime.tv_sec = (int64_t)node->atime;
  stx->stx_mtime.tv_sec = (int64_t)node->mtime;
  stx->stx_ctime.tv_sec = (int64_t)node->ctime;
  // stx_btime not supported by VFS, left as 0

  return 0;
}

// Linux getdents64 structure (binary compatible with musl/glibc)
struct linux_dirent64 {
  uint64_t d_ino;    // Inode number
  uint64_t d_off;    // Offset to next entry
  uint16_t d_reclen; // Size of this dirent
  uint8_t d_type;    // File type (DT_REG, DT_DIR, etc.)
  char d_name[];     // Filename (null-terminated)
};

// DT_* constants from Linux dirent.h
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

// getdents64(fd, dirp, count) - read directory entries
static uint64_t sys_getdents64(uint64_t fd, uint64_t dirp, uint64_t count,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd])
    return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  if ((node->flags & 0xFF) != FS_DIRECTORY)
    return (uint64_t)-20; // ENOTDIR

  uint8_t *buf = (uint8_t *)dirp;
  if (!buf)
    return (uint64_t)-14; // EFAULT
  if (!is_user_ptr((uint64_t)buf))
    return (uint64_t)-14; // EFAULT

  size_t written = 0;
  uint32_t index = t->fd_offsets[fd]; // Start from saved position

  while (1) {
    struct dirent *de = vfs_readdir(node, index);
    if (!de)
      break; // No more entries

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
      case FS_FILE:
        entry->d_type = DT_REG;
        break;
      case FS_DIRECTORY:
        entry->d_type = DT_DIR;
        break;
      case FS_CHARDEV:
        entry->d_type = DT_CHR;
        break;
      case FS_BLOCKDEV:
        entry->d_type = DT_BLK;
        break;
      case FS_SYMLINK:
        entry->d_type = DT_LNK;
        break;
      default:
        entry->d_type = DT_UNKNOWN;
        break;
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

// Stub for poll(2) - used by musl stdio for detecting tty
struct pollfd {
  int fd;
  short events;
  short revents;
};

static uint64_t sys_poll(uint64_t fds_ptr, uint64_t nfds, uint64_t timeout,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *t = sched_get_current();
  struct pollfd *fds = (struct pollfd *)fds_ptr;
  if (!t || !fds)
    return (uint64_t)-1;

  uint64_t start_ticks = pit_get_ticks();
  uint64_t timeout_ticks = (timeout == (uint64_t)-1) ? 0 : (timeout / 10);
  if (timeout != (uint64_t)-1 && timeout_ticks == 0 && timeout > 0)
    timeout_ticks = 1;

  while (1) {
    int count = 0;
    for (uint64_t i = 0; i < nfds; i++) {
      fds[i].revents = 0;
      int fd = fds[i].fd;

      if (fd < 0 || fd >= MAX_FDS || !t->fds[fd]) {
        fds[i].revents = POLLNVAL;
        count++;
        continue;
      }

      int revents = vfs_poll(t->fds[fd], fds[i].events);
      if (revents) {
        klog_puts("[POLL] fd=");
        klog_uint64(fd);
        klog_puts(" req=");
        klog_uint64(fds[i].events);
        klog_puts(" got=");
        klog_uint64(revents);
        klog_puts("\n");
        fds[i].revents = revents;
        count++;
      }
    }

    if (count > 0)
      return (uint64_t)count;
    if (timeout == 0)
      return 0;

    uint64_t current_ticks = pit_get_ticks();
    if (timeout != (uint64_t)-1 &&
        (current_ticks - start_ticks) >= timeout_ticks)
      return 0;

    // Block the thread. We'll be woken up by a driver's wait_queue_wake_all
    // or by the scheduler timer if we set wakeup_ticks.
    t->state = THREAD_BLOCKED;
    if (timeout != (uint64_t)-1) {
      t->wakeup_ticks = start_ticks + timeout_ticks;
    } else {
      t->wakeup_ticks = 0;
    }

    if (net_poll()) {
      t->state = THREAD_READY;
      t->wakeup_ticks = 0;
    }
    sched_yield();
  }
}

typedef struct {
  uint64_t fds_bits[1024 / 64];
} fd_set_t;

struct k_timeval {
  int64_t tv_sec;
  int64_t tv_usec;
};

static uint64_t sys_select(uint64_t nfds, uint64_t readfds_ptr,
                           uint64_t writefds_ptr, uint64_t exceptfds_ptr,
                           uint64_t timeout_ptr, uint64_t a5) {
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t) return (uint64_t)-1;

  fd_set_t *readfds = (fd_set_t *)readfds_ptr;
  fd_set_t *writefds = (fd_set_t *)writefds_ptr;
  fd_set_t *exceptfds = (fd_set_t *)exceptfds_ptr;
  struct k_timeval *tv = (struct k_timeval *)timeout_ptr;

  uint64_t timeout_ms = (uint64_t)-1;
  if (tv) {
    timeout_ms = (tv->tv_sec * 1000) + (tv->tv_usec / 1000);
  }

  struct pollfd pfds[MAX_FDS];
  int pcount = 0;
  int map[MAX_FDS];

  for (int i = 0; i < (int)nfds && i < MAX_FDS; i++) {
    int events = 0;
    if (readfds && (readfds->fds_bits[i / 64] & (1ULL << (i % 64))))
      events |= POLLIN;
    if (writefds && (writefds->fds_bits[i / 64] & (1ULL << (i % 64))))
      events |= POLLOUT;
    if (exceptfds && (exceptfds->fds_bits[i / 64] & (1ULL << (i % 64))))
      events |= POLLERR;

    if (events) {
      pfds[pcount].fd = i;
      pfds[pcount].events = events;
      pfds[pcount].revents = 0;
      map[pcount] = i;
      pcount++;
    }
  }

  if (pcount == 0) {
    if (timeout_ms == 0) return 0;
    pit_sleep(timeout_ms);
    return 0;
  }

  uint64_t ret = sys_poll((uint64_t)pfds, (uint64_t)pcount, timeout_ms, 0, 0, 0);

  if (ret > 0 && ret != (uint64_t)-1) {
    if (readfds) memset(readfds, 0, sizeof(fd_set_t));
    if (writefds) memset(writefds, 0, sizeof(fd_set_t));
    if (exceptfds) memset(exceptfds, 0, sizeof(fd_set_t));

    int ready = 0;
    for (int i = 0; i < pcount; i++) {
      int fd = map[i];
      bool is_ready = false;
      if (pfds[i].revents & POLLIN) {
        if (readfds) readfds->fds_bits[fd / 64] |= (1ULL << (fd % 64));
        is_ready = true;
      }
      if (pfds[i].revents & POLLOUT) {
        if (writefds) writefds->fds_bits[fd / 64] |= (1ULL << (fd % 64));
        is_ready = true;
      }
      if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        if (exceptfds) exceptfds->fds_bits[fd / 64] |= (1ULL << (fd % 64));
        is_ready = true;
      }
      if (is_ready) ready++;
    }
    return (uint64_t)ready;
  }

  return ret;
}

// Simple xorshift64 PRNG state
static uint64_t prng_state = 0;

static void prng_seed(void) {
  if (prng_state != 0)
    return;

  // Use RDTSC for initial entropy
  uint64_t tsc;
  __asm__ volatile("rdtsc" : "=A"(tsc));

  // Mix with PIT ticks if available
  uint64_t ticks = pit_get_ticks();

  // Combine sources
  prng_state = tsc ^ (ticks << 32) ^ 0xDEADBEEFCAFEBABEULL;

  // Ensure non-zero
  if (prng_state == 0)
    prng_state = 1;
}

static uint64_t xorshift64(void) {
  uint64_t x = prng_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  prng_state = x;
  return x;
}

// getrandom(2) - syscall 318
static uint64_t sys_getrandom(uint64_t buf_ptr, uint64_t buflen, uint64_t flags,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)flags;
  (void)a3;
  (void)a4;
  (void)a5;

  if (!buf_ptr)
    return (uint64_t)-14; // EFAULT
  if (!is_user_ptr(buf_ptr))
    return (uint64_t)-14; // EFAULT
  if (buflen == 0)
    return 0;

  // Initialize PRNG if needed
  prng_seed();

  uint8_t *buf = (uint8_t *)buf_ptr;
  uint64_t written = 0;

  // Fill buffer with random bytes
  while (written < buflen) {
    uint64_t rand_val = xorshift64();

    // Write up to 8 bytes at a time
    for (int i = 0; i < 8 && written < buflen; i++) {
      buf[written++] = (uint8_t)(rand_val & 0xFF);
      rand_val >>= 8;
    }
  }

  return written;
}

// ── sys_mkdir: mkdir(pathname, mode) — syscall 83 ──────────────────────────
static uint64_t sys_mkdir(uint64_t pathname, uint64_t mode, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)pathname;
  if (!path)
    return (uint64_t)-14; // EFAULT

  // Split path into parent directory + new dir name
  char parent_path[128];
  char dir_name[128];
  size_t len = strlen(path);
  if (len == 0 || len >= sizeof(dir_name))
    return (uint64_t)-14;

  const char *slash = 0;
  for (const char *p = path; *p; p++)
    if (*p == '/')
      slash = p;

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
    size_t dlen = strlen(slash + 1);
    if (dlen == 0 || dlen >= sizeof(dir_name))
      return (uint64_t)-22; // EINVAL
    strcpy(dir_name, slash + 1);
  } else {
    strcpy(dir_name, path);
  }

  if (!parent || (parent->flags & 0x07) != FS_DIRECTORY)
    return (uint64_t)-20; // ENOTDIR

  if (vfs_mkdir(parent, dir_name, (uint16_t)mode) != 0)
    return (uint64_t)-17; // EEXIST

  return 0;
}

// ── sys_readv: readv(fd, iov, iovcnt) — syscall 19 ──────────────────────────
static uint64_t sys_readv(uint64_t fd, uint64_t iov_u, uint64_t iovcnt,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  if (iovcnt == 0)
    return 0;
  if (iovcnt > 1024)
    return (uint64_t)-22; // EINVAL

  struct user_iovec *iov = (struct user_iovec *)iov_u;
  size_t total = 0;

  for (uint64_t i = 0; i < iovcnt; i++) {
    uint64_t base = iov[i].iov_base;
    uint64_t len = iov[i].iov_len;
    if (len == 0)
      continue;
    if (!is_user_ptr(base))
      return (uint64_t)-14; // EFAULT

    // Reuse sys_read logic
    struct thread *t = sched_get_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd])
      return (uint64_t)-9; // EBADF

    vfs_node_t *node = t->fds[fd];
    uint32_t bytes_read =
        vfs_read(node, t->fd_offsets[fd], len, (uint8_t *)base);
    t->fd_offsets[fd] += bytes_read;
    total += bytes_read;
    if (bytes_read < len)
      break; // Short read
  }
  return total;
}

// ── sys_pipe2: pipe2(pipefd, flags) — syscall 293 ────────────────────────────
static uint64_t sys_pipe2(uint64_t pipefd_ptr, uint64_t flags, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)flags;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  int *pipefd = (int *)pipefd_ptr;
  if (!pipefd)
    return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  // Allocate two file descriptors
  int fd_read = alloc_fd(t);
  if (fd_read < 0)
    return (uint64_t)-24;       // EMFILE
  t->fds[fd_read] = (void *)-1; // Reserve it

  int fd_write = alloc_fd(t);
  if (fd_write < 0) {
    t->fds[fd_read] = NULL;
    return (uint64_t)-24; // EMFILE
  }

  // Build pipe buffer node directly in kernel heap (avoids ext2 create issues)
  vfs_node_t *pipe_node = kmalloc(sizeof(vfs_node_t));
  if (!pipe_node) {
    t->fds[fd_read] = NULL;
    return (uint64_t)-12; // ENOMEM
  }
  memset(pipe_node, 0, sizeof(vfs_node_t));

  ramfs_file_t *pipe_buf = kmalloc(sizeof(ramfs_file_t));
  if (!pipe_buf) {
    kfree(pipe_node);
    t->fds[fd_read] = NULL;
    return (uint64_t)-12;
  }
  pipe_buf->data = NULL;
  pipe_buf->capacity = 0;

  pipe_node->flags = FS_FILE;
  pipe_node->device = pipe_buf;
  pipe_node->length = 0;
  pipe_node->read = ramfs_read;
  pipe_node->write = ramfs_write;

  // Both fds point to the same node; read offset and write offset tracked
  // separately
  t->fds[fd_read] = pipe_node;
  t->fds[fd_write] = pipe_node;
  t->fd_offsets[fd_read] = 0;
  t->fd_offsets[fd_write] = 0;

  pipefd[0] = fd_read;
  pipefd[1] = fd_write;

  return 0;
}

// ── sys_pipe: pipe(pipefd) — syscall 22 ──────────────────────────────────────
static uint64_t sys_pipe(uint64_t pipefd_ptr, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  return sys_pipe2(pipefd_ptr, 0, a2, a3, a4, a5);
}

// ── sys_access: access(pathname, mode) — syscall 21 ──────────────────────────
static uint64_t do_sys_access(int dirfd, const char *path, uint64_t mode,
                              int flags) {
  (void)flags;
  if (!path)
    return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  vfs_node_t *node = NULL;

  if (path[0] == '/') {
    node = vfs_resolve_path_at(fs_root, path);
  } else if (strncmp(path, "/dev/", 5) == 0) {
    node = fb_lookup_device((char *)path + 5);
  }

  if (!node) {
    vfs_node_t *base_dir = fs_root;
    if (dirfd == AT_FDCWD) {
      if (t && t->cwd_path[0]) {
        base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
        if (!base_dir)
          base_dir = fs_root;
      }
    } else {
      if (dirfd < 0 || dirfd >= MAX_FDS || !t->fds[dirfd])
        return (uint64_t)-9; // EBADF
      base_dir = t->fds[dirfd];
      if ((base_dir->flags & 0xFF) != FS_DIRECTORY)
        return (uint64_t)-20; // ENOTDIR
    }
    node = vfs_resolve_path_at(base_dir, path);
  }

  if (!node) {
    return (uint64_t)-2; // ENOENT
  }

  // POSIX mode check.
  // mode bits: F_OK = 0, X_OK = 1, W_OK = 2, R_OK = 4.
  if (mode != 0) {
    bool can_read = (node->mask & 0444) != 0;
    bool can_write = (node->mask & 0222) != 0;
    bool can_exec = (node->mask & 0111) != 0;

    if ((mode & 4) && !can_read)
      return (uint64_t)-13; // EACCES
    if ((mode & 2) && !can_write)
      return (uint64_t)-13; // EACCES
    if ((mode & 1) && !can_exec)
      return (uint64_t)-13; // EACCES
  }

  return 0;
}

static uint64_t sys_access(uint64_t pathname_ptr, uint64_t mode, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return do_sys_access(AT_FDCWD, (const char *)pathname_ptr, mode, 0);
}

static uint64_t sys_faccessat2(uint64_t dirfd, uint64_t pathname_ptr,
                               uint64_t mode, uint64_t flags, uint64_t a4,
                               uint64_t a5) {
  (void)a4;
  (void)a5;
  return do_sys_access((int)dirfd, (const char *)pathname_ptr, mode,
                       (int)flags);
}

// ── sys_newfstatat: fstatat(dirfd, pathname, statbuf, flags) — syscall 262 ──
#define AT_EMPTY_PATH 0x1000

static uint64_t sys_newfstatat(uint64_t dirfd, uint64_t pathname_ptr,
                               uint64_t statbuf_ptr, uint64_t flags,
                               uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  const char *path = (const char *)pathname_ptr;
  struct kstat *ks = (struct kstat *)statbuf_ptr;

  if (!ks)
    return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  // AT_EMPTY_PATH with empty string: behave like fstat(dirfd)
  if ((flags & AT_EMPTY_PATH) && path && path[0] == '\0') {
    if ((int)dirfd < 0 || dirfd >= MAX_FDS || !t->fds[dirfd])
      return (uint64_t)-9; // EBADF
    fill_kstat(ks, t->fds[dirfd]);
    return 0;
  }

  if (!path)
    return (uint64_t)-14; // EFAULT

  // Resolve the base directory
  vfs_node_t *base_dir = fs_root;
  if (path[0] != '/') {
    if ((int)dirfd == AT_FDCWD) {
      // Use thread CWD
      if (t->cwd_path[0]) {
        base_dir = vfs_resolve_path_at(fs_root, t->cwd_path);
        if (!base_dir)
          base_dir = fs_root;
      }
    } else {
      if ((int)dirfd < 0 || dirfd >= MAX_FDS || !t->fds[dirfd])
        return (uint64_t)-9; // EBADF
      base_dir = t->fds[dirfd];
      if ((base_dir->flags & 0xFF) != FS_DIRECTORY)
        return (uint64_t)-20; // ENOTDIR
    }
  }

  vfs_node_t *node = vfs_resolve_path_at(base_dir, path);
  if (!node) {
    return (uint64_t)-2; // ENOENT
  }

  fill_kstat(ks, node);
  return 0;
}

// ── sys_openat: openat(dirfd, pathname, flags, mode) — syscall 257 ──────────
static uint64_t sys_openat(uint64_t dirfd, uint64_t pathname_ptr,
                           uint64_t flags, uint64_t mode, uint64_t a4,
                           uint64_t a5) {
  (void)a4;
  (void)a5;
  return do_sys_open((int)dirfd, (const char *)pathname_ptr, flags, mode);
}

// ── Helper: split path into parent dir + basename ───────────────────────────
// Returns the parent VFS node and writes the basename into 'name_out'.
// Returns NULL on failure.
static vfs_node_t *resolve_parent_and_name(const char *path, char *name_out,
                                           size_t name_size) {
  if (!path || !name_out || name_size == 0)
    return NULL;

  size_t len = strlen(path);
  if (len == 0 || len >= 256)
    return NULL;

  struct thread *t = sched_get_current();
  vfs_node_t *base = fs_root;
  if (t && path[0] != '/' && t->cwd_path[0]) {
    base = vfs_resolve_path_at(fs_root, t->cwd_path);
    if (!base)
      base = fs_root;
  }

  // Find last slash
  const char *last_slash = NULL;
  for (const char *p = path; *p; p++)
    if (*p == '/')
      last_slash = p;

  vfs_node_t *parent;
  const char *basename;

  if (last_slash) {
    size_t parent_len = (size_t)(last_slash - path);
    if (parent_len == 0) {
      parent = fs_root;
    } else {
      char parent_path[256];
      if (parent_len >= sizeof(parent_path))
        return NULL;
      memcpy(parent_path, path, parent_len);
      parent_path[parent_len] = '\0';
      parent = vfs_resolve_path_at(base, parent_path);
    }
    basename = last_slash + 1;
  } else {
    parent = base;
    basename = path;
  }

  if (!parent || (parent->flags & 0x07) != FS_DIRECTORY)
    return NULL;

  size_t blen = strlen(basename);
  if (blen == 0 || blen >= name_size)
    return NULL;

  strcpy(name_out, basename);
  return parent;
}

// ── sys_symlink: symlink(target, linkpath) — syscall 88 ──────────────────────
static uint64_t sys_symlink(uint64_t target_ptr, uint64_t linkpath_ptr,
                            uint64_t a2, uint64_t a3, uint64_t a4,
                            uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *target = (const char *)target_ptr;
  const char *linkpath = (const char *)linkpath_ptr;

  if (!target || !linkpath)
    return (uint64_t)-14; // EFAULT

  char link_name[128];
  vfs_node_t *parent =
      resolve_parent_and_name(linkpath, link_name, sizeof(link_name));
  if (!parent)
    return (uint64_t)-2; // ENOENT

  // Check if it already exists
  if (vfs_finddir(parent, link_name)) {
    return (uint64_t)-17; // EEXIST
  }

  // A local buffer for target string
  char target_buf[256];
  size_t t_len = strlen(target);
  if (t_len == 0 || t_len >= sizeof(target_buf))
    return (uint64_t)-14;
  strcpy(target_buf, target);

  int ret = vfs_symlink(parent, link_name, target_buf);
  if (ret != 0)
    return (uint64_t)-1;
  return 0;
}

// ── sys_unlink: unlink(pathname) — syscall 87 ────────────────────────────────
static uint64_t sys_unlink(uint64_t pathname_ptr, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)pathname_ptr;
  if (!path)
    return (uint64_t)-14; // EFAULT

  char name[128];
  vfs_node_t *parent = resolve_parent_and_name(path, name, sizeof(name));
  if (!parent)
    return (uint64_t)-2; // ENOENT

  // Check that the target exists and is not a directory
  vfs_node_t *target = vfs_finddir(parent, name);
  if (!target)
    return (uint64_t)-2; // ENOENT
  if ((target->flags & 0x07) == FS_DIRECTORY)
    return (uint64_t)-21; // EISDIR

  // Invalidate any open fds pointing to this node
  struct thread *t = sched_get_current();
  if (t) {
    for (int i = 0; i < MAX_FDS; i++) {
      if (t->fds[i] == target) {
        t->fds[i] = NULL;
        t->fd_offsets[i] = 0;
      }
    }
  }

  int ret = vfs_unlink(parent, name);
  if (ret != 0)
    return (uint64_t)-1; // Generic error

  return 0;
}

// ── sys_rmdir: rmdir(pathname) — syscall 84 ──────────────────────────────────
static uint64_t sys_rmdir(uint64_t pathname_ptr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)pathname_ptr;
  if (!path)
    return (uint64_t)-14; // EFAULT

  char name[128];
  vfs_node_t *parent = resolve_parent_and_name(path, name, sizeof(name));
  if (!parent)
    return (uint64_t)-2; // ENOENT

  vfs_node_t *target = vfs_finddir(parent, name);
  if (!target)
    return (uint64_t)-2; // ENOENT
  if ((target->flags & 0x07) != FS_DIRECTORY)
    return (uint64_t)-20; // ENOTDIR

  int ret = vfs_rmdir(parent, name);
  if (ret != 0)
    return (uint64_t)-1;

  return 0;
}

// ── sys_chmod: chmod(pathname, mode) — syscall 90 ────────────────────────────
static uint64_t sys_chmod(uint64_t pathname_ptr, uint64_t mode, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)pathname_ptr;
  if (!path)
    return (uint64_t)-14;

  struct thread *t = sched_get_current();
  vfs_node_t *base = fs_root;
  if (t && path[0] != '/' && t->cwd_path[0]) {
    base = vfs_resolve_path_at(fs_root, t->cwd_path);
    if (!base)
      base = fs_root;
  }

  vfs_node_t *node = vfs_resolve_path_at(base, path);
  if (!node)
    return (uint64_t)-2;

  int ret = vfs_chmod(node, (uint16_t)mode);
  if (ret != 0)
    return (uint64_t)-1;
  return 0;
}

// ── sys_chown: chown(pathname, owner, group) — syscall 92 ────────────────────
static uint64_t sys_chown(uint64_t pathname_ptr, uint64_t owner, uint64_t group,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)pathname_ptr;
  if (!path)
    return (uint64_t)-14;

  struct thread *t = sched_get_current();
  vfs_node_t *base = fs_root;
  if (t && path[0] != '/' && t->cwd_path[0]) {
    base = vfs_resolve_path_at(fs_root, t->cwd_path);
    if (!base)
      base = fs_root;
  }

  vfs_node_t *node = vfs_resolve_path_at(base, path);
  if (!node)
    return (uint64_t)-2;

  uint32_t uid = (owner == (uint64_t)-1) ? node->uid : (uint32_t)owner;
  uint32_t gid = (group == (uint64_t)-1) ? node->gid : (uint32_t)group;

  int ret = vfs_chown(node, uid, gid);
  if (ret != 0)
    return (uint64_t)-1;
  return 0;
}

// ── sys_rename: rename(oldpath, newpath) — syscall 82 ────────────────────────
static uint64_t sys_rename(uint64_t oldpath_ptr, uint64_t newpath_ptr,
                           uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *oldpath = (const char *)oldpath_ptr;
  const char *newpath = (const char *)newpath_ptr;
  if (!oldpath || !newpath)
    return (uint64_t)-14; // EFAULT

  char old_name[128], new_name[128];
  vfs_node_t *old_parent =
      resolve_parent_and_name(oldpath, old_name, sizeof(old_name));
  vfs_node_t *new_parent =
      resolve_parent_and_name(newpath, new_name, sizeof(new_name));

  if (!old_parent)
    return (uint64_t)-2; // ENOENT (source path invalid)
  if (!new_parent)
    return (uint64_t)-2; // ENOENT (dest path invalid)

  // Check source exists
  vfs_node_t *source = vfs_finddir(old_parent, old_name);
  if (!source)
    return (uint64_t)-2; // ENOENT

  // Same directory rename (most common case, and what TCC uses)
  // Compare by inode, not pointer — ext2 finddir allocates new nodes per lookup
  if (old_parent == new_parent || old_parent->inode == new_parent->inode) {
    int ret = vfs_rename(old_parent, old_name, new_name);
    if (ret != 0)
      return (uint64_t)-1;
    return 0;
  }

  // Cross-directory rename is not supported in this simple VFS
  return (uint64_t)-18; // EXDEV
}

// ── sys_readlink: readlink(pathname, buf, bufsiz) — syscall 89 ───────────────
static uint64_t sys_readlink(uint64_t pathname_ptr, uint64_t buf_ptr,
                             uint64_t bufsiz, uint64_t a3, uint64_t a4,
                             uint64_t a5) {
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)pathname_ptr;
  char *buf = (char *)buf_ptr;
  if (!path || !buf)
    return (uint64_t)-14; // EFAULT
  if (bufsiz == 0)
    return (uint64_t)-22; // EINVAL

  // Special case: /proc/self/exe — TCC and other tools read this
  if (strcmp(path, "/proc/self/exe") == 0) {
    // We don't track per-thread executable paths yet, so return a generic path
    const char *exe_path = "/init";
    size_t len = strlen(exe_path);
    if (len > bufsiz)
      len = bufsiz;
    memcpy(buf, exe_path, len);
    return len; // readlink returns bytes written, NOT null-terminated
  }

  // Resolve the symlink node
  struct thread *t = sched_get_current();
  vfs_node_t *base = fs_root;
  if (t && path[0] != '/' && t->cwd_path[0]) {
    base = vfs_resolve_path_at(fs_root, t->cwd_path);
    if (!base)
      base = fs_root;
  }

  vfs_node_t *node = vfs_resolve_path_at(base, path);
  if (!node)
    return (uint64_t)-2; // ENOENT

  // Must be a symlink
  if ((node->flags & 0xFF) != FS_SYMLINK)
    return (uint64_t)-22; // EINVAL — not a symlink

  int ret = vfs_readlink(node, buf, (uint32_t)bufsiz);
  if (ret < 0)
    return (uint64_t)-22; // EINVAL

  return (uint64_t)ret;
}

// ── sys_dup: dup(oldfd) — syscall 32
// ──────────────────────────────────────────
static uint64_t sys_dup(uint64_t oldfd, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || oldfd >= MAX_FDS || !t->fds[oldfd])
    return (uint64_t)-9; // EBADF

  int newfd = alloc_fd(t);
  if (newfd < 0)
    return (uint64_t)-24; // EMFILE

  t->fds[newfd] = t->fds[oldfd];
  t->fd_offsets[newfd] = t->fd_offsets[oldfd];

  return newfd;
}

// ── sys_dup2: dup2(oldfd, newfd) — syscall 33 ────────────────────────────────
static uint64_t sys_dup2(uint64_t oldfd, uint64_t newfd, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *t = sched_get_current();
  if (!t || oldfd >= MAX_FDS || !t->fds[oldfd])
    return (uint64_t)-9; // EBADF

  if (newfd >= MAX_FDS)
    return (uint64_t)-9; // EBADF

  if (oldfd == newfd)
    return newfd;

  // If newfd is already open, close it
  if (t->fds[newfd]) {
    vfs_close(t->fds[newfd]);
    t->fds[newfd] = NULL;
    t->fd_offsets[newfd] = 0;
  }

  t->fds[newfd] = t->fds[oldfd];
  t->fd_offsets[newfd] = t->fd_offsets[oldfd];

  return newfd;
}

static uint64_t sys_utimensat(uint64_t dirfd, uint64_t pathname, uint64_t times,
                               uint64_t flags, uint64_t a4, uint64_t a5) {
  (void)dirfd;
  (void)pathname;
  (void)times;
  (void)flags;
  (void)a4;
  (void)a5;
  return 0; // successfully mocked
}

static uint64_t sys_futimesat(uint64_t dirfd, uint64_t pathname, uint64_t times,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)dirfd;
  (void)pathname;
  (void)times;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0; // successfully mocked
}

static uint64_t sys_utimes(uint64_t pathname, uint64_t times, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)pathname;
  (void)times;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  return 0; // successfully mocked
}

void syscall_register_io(void) {
  syscall_register(SYS_READ, sys_read);
  syscall_register(SYS_WRITE, sys_write);
  syscall_register(SYS_READV, sys_readv);
  syscall_register(SYS_WRITEV, sys_writev);
  syscall_register(SYS_IOCTL, sys_ioctl);
  syscall_register(SYS_OPEN, sys_open);
  syscall_register(SYS_CLOSE, sys_close);
  syscall_register(SYS_LSEEK, sys_lseek);
  syscall_register(SYS_MKDIR, sys_mkdir);
  syscall_register(SYS_FTRUNCATE, sys_ftruncate);
  syscall_register(SYS_FCNTL, sys_fcntl);
  syscall_register(SYS_STAT, sys_stat);
  syscall_register(SYS_FSTAT, sys_fstat);
  syscall_register(SYS_GETDENTS64, sys_getdents64);
  syscall_register(SYS_POLL, sys_poll);
  syscall_register(SYS_SELECT, sys_select);
  syscall_register(SYS_PIPE, sys_pipe);
  syscall_register(SYS_PIPE2, sys_pipe2);
  syscall_register(SYS_GETRANDOM, sys_getrandom);
  syscall_register(SYS_ACCESS, sys_access);
  syscall_register(SYS_FACCESSAT2, sys_faccessat2);
  syscall_register(SYS_OPENAT, sys_openat);
  syscall_register(SYS_NEWFSTATAT, sys_newfstatat);
  syscall_register(SYS_UNLINK, sys_unlink);
  syscall_register(SYS_RENAME, sys_rename);
  syscall_register(SYS_SYMLINK, sys_symlink);
  syscall_register(SYS_READLINK, sys_readlink);
  syscall_register(SYS_DUP, sys_dup);
  syscall_register(SYS_DUP2, sys_dup2);
  syscall_register(SYS_RMDIR, sys_rmdir);
  syscall_register(SYS_CHMOD, sys_chmod);
  syscall_register(SYS_CHOWN, sys_chown);
  syscall_register(SYS_STATX, sys_statx);
  syscall_register(SYS_UTIMENSAT, sys_utimensat);
  syscall_register(SYS_FUTIMESAT, sys_futimesat);
  syscall_register(SYS_UTIMES, sys_utimes);

  // Initialize console termios with standard defaults:
  console_termios.c_lflag = 0x0000000b; // ISIG | ICANON | ECHO
  console_termios.c_iflag = 0x00000100; // ICRNL (0x100)
  console_termios.c_oflag = 0x00000005; // OPOST (1) | ONLCR (4)
}