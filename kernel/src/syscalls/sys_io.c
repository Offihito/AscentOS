// ── I/O Syscalls: read, write, close ────────────────────────────────────────
#include "syscall.h"
#include "console/klog.h"
#include <stdint.h>

extern void console_putchar(char c);

static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)fd; (void)buf; (void)count;
  (void)a3; (void)a4; (void)a5;
  return 0; // Return 0 bytes read (EOF) for now.
}

static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  const char *str = (const char *)buf;

  if (fd == 1 || fd == 2) {
    for (uint64_t i = 0; i < count; i++) {
      console_putchar(str[i]);
    }
    return count;
  }
  return (uint64_t)-1;
}

static uint64_t sys_close(uint64_t fd, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)fd; (void)a1; (void)a2;
  (void)a3; (void)a4; (void)a5;
  return 0; // No-op success for now.
}

void syscall_register_io(void) {
  syscall_register(SYS_READ,  sys_read);
  syscall_register(SYS_WRITE, sys_write);
  syscall_register(SYS_CLOSE, sys_close);
}
