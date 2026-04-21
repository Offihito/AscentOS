#include "syscall.h"
#include "../console/klog.h"
#include "../cpu/msr.h"
#include <stdint.h>

extern void syscall_entry(void);

// ── Syscall dispatch tables ─────────────────────────────────────────────────
static syscall_handler_t syscall_table[MAX_SYSCALL] = {0};
static syscall_raw_handler_t raw_syscall_table[MAX_SYSCALL] = {0};

void syscall_register(int num, syscall_handler_t handler) {
  if (num >= 0 && num < MAX_SYSCALL) {
    syscall_table[num] = handler;
  }
}

void syscall_register_raw(int num, syscall_raw_handler_t handler) {
  if (num >= 0 && num < MAX_SYSCALL) {
    raw_syscall_table[num] = handler;
  }
}

// ── Dispatcher (called from syscall_entry.asm) ──────────────────────────────
void syscall_dispatcher(struct syscall_regs *regs) {
  if (regs->rax >= MAX_SYSCALL) {
    klog_puts("\n[SYSCALL] Unimplemented syscall: ");
    klog_uint64(regs->rax);
    klog_puts("\n");
    regs->rax = (uint64_t)-38; // ENOSYS
    return;
  }

  // Check raw handlers first (e.g. fork needs the full register frame)
  if (raw_syscall_table[regs->rax]) {
    syscall_raw_handler_t raw_handler = raw_syscall_table[regs->rax];
    regs->rax = raw_handler(regs);
    return;
  }

  if (!syscall_table[regs->rax]) {
    klog_puts("\n[SYSCALL] Unimplemented syscall: ");
    klog_uint64(regs->rax);
    klog_puts("\n");
    regs->rax = (uint64_t)-38; // ENOSYS
    return;
  }

  syscall_handler_t handler = syscall_table[regs->rax];
  regs->rax =
      handler(regs->rdi, regs->rsi, regs->rdx, regs->r10, regs->r8, regs->r9);

  // Delivery signals before returning to usermode
  signal_deliver_syscall(regs);
}

// ── Core initialization ────────────────────────────────────────────────────
void syscall_init(void) {
  // Register all syscall subsystems
  syscall_register_io();
  syscall_register_process();
  syscall_register_mm();
  syscall_register_arch();
  syscall_register_signal();
  syscall_register_net();

  // 1. Enable System Call Extensions and No-Execute
  uint64_t efer = rdmsr(IA32_EFER);
  efer |= IA32_EFER_SCE | (1ULL << 11); // Set NXE
  wrmsr(IA32_EFER, efer);

  // 2. Configure STAR:
  // KCode=0x08, KData=0x10, UCode32=0x18|3, UData=0x20|3, UCode64=0x28|3
  // SYSRET base (63:48) points to Index 3 (UCode32).
  // CS = base + 16 = Index 5 (UCode64), SS = base + 8 = Index 4 (UData).
  uint64_t star = ((uint64_t)0x1B << 48) | ((uint64_t)0x08 << 32);
  wrmsr(IA32_STAR, star);

  // 3. Configure LSTAR with the syscall entry point
  wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);

  // 4. Configure FMASK (Mask interrupts IF=0x200 while in syscall)
  wrmsr(IA32_FMASK, 0x200);

  klog_puts("[OK] Syscall Infrastructure (MSRs) initialized.\n");
}
