// ── Process Syscalls: exit ──────────────────────────────────────────────────
#include "syscall.h"
#include "console/klog.h"
#include <stdint.h>

// Defined in process.c — the saved kernel context from before entering Ring 3
typedef uint64_t kernel_jmp_buf[8];
extern kernel_jmp_buf process_return_ctx;
extern void kernel_longjmp(kernel_jmp_buf buf, int val) __attribute__((noreturn));

static uint64_t sys_exit(uint64_t status, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

  klog_puts("\n[SYSCALL] Process exited with status: ");
  klog_uint64(status);
  klog_puts("\n");

  // Return to the shell by restoring kernel context saved in process_exec.
  kernel_longjmp(process_return_ctx, 1);

  // Unreachable
  return 0;
}

void syscall_register_process(void) {
  syscall_register(SYS_EXIT, sys_exit);
}
