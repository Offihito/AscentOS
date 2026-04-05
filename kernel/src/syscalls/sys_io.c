// ── I/O Syscalls: read, write, close, open, lseek ────────────────────────────
#include "syscall.h"
#include "console/klog.h"
#include "sched/sched.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include <stdint.h>

extern void console_putchar(char c);

static int alloc_fd(struct thread *t) {
  for (int i = 0; i < MAX_FDS; i++) {
    if (t->fds[i] == NULL) {
      return i;
    }
  }
  return -1; // ENFILE
}

static uint64_t sys_open(uint64_t path_ptr, uint64_t flags, uint64_t mode,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)flags; (void)mode; (void)a3; (void)a4; (void)a5;
  const char *path = (const char *)path_ptr;
  if (!path) return (uint64_t)-14; // EFAULT

  struct thread *t = sched_get_current();
  if (!t) return (uint64_t)-1;

  int fd = alloc_fd(t);
  if (fd < 0) return (uint64_t)-24; // EMFILE

  vfs_node_t *node = vfs_resolve_path(path);
  if (!node) {
      klog_puts("[SYSCALL] sys_open failed to find: ");
      klog_puts(path);
      klog_puts("\n");
      return (uint64_t)-2; // ENOENT
  }

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

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  const char *str = (const char *)buf;

  // Fallback map 1 and 2 to console if not properly mapped.
  if (fd == 1 || fd == 2) {
    for (uint64_t i = 0; i < count; i++) {
        console_putchar(str[i]);
    }
    return count;
  }

  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];
  uint32_t bytes_written = vfs_write(node, t->fd_offsets[fd], count, (uint8_t *)buf);
  t->fd_offsets[fd] += bytes_written;

  return bytes_written;
}

static uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9; // EBADF

  vfs_node_t *node = t->fds[fd];

  // whence: 0 = SEEK_SET, 1 = SEEK_CUR, 2 = SEEK_END
  int64_t new_offset = 0;
  if (whence == 0) {
      new_offset = offset;
  } else if (whence == 1) {
      new_offset = t->fd_offsets[fd] + offset;
  } else if (whence == 2) {
      new_offset = node->length + offset;
  } else {
      return (uint64_t)-22; // EINVAL
  }

  if (new_offset < 0) return (uint64_t)-22; // EINVAL

  t->fd_offsets[fd] = new_offset;
  return new_offset;
}

static uint64_t sys_close(uint64_t fd, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
  struct thread *t = sched_get_current();
  if (!t || fd >= MAX_FDS || !t->fds[fd]) return (uint64_t)-9; // EBADF

  vfs_close(t->fds[fd]);
  t->fds[fd] = NULL;
  t->fd_offsets[fd] = 0;
  
  return 0; 
}

void syscall_register_io(void) {
  syscall_register(SYS_READ,  sys_read);
  syscall_register(SYS_WRITE, sys_write);
  syscall_register(SYS_OPEN,  sys_open);
  syscall_register(SYS_CLOSE, sys_close);
  syscall_register(SYS_LSEEK, sys_lseek);
}
