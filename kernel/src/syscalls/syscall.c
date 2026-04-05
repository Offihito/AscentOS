#include "syscall.h"
#include "console/klog.h"
#include "cpu/msr.h"
#include <stdint.h>

extern void syscall_entry(void);

// ── Register state pushed by syscall_entry.asm ──────────────────────────────
struct syscall_regs {
  uint64_t rdi, rsi, rdx, r10, r8, r9, rax, rbx, rbp, r12, r13, r14, r15;
  uint64_t rip, rflags, rsp;
} __attribute__((packed));

// ── Syscall dispatch table ──────────────────────────────────────────────────
static syscall_handler_t syscall_table[MAX_SYSCALL] = {0};

void syscall_register(int num, syscall_handler_t handler) {
  if (num >= 0 && num < MAX_SYSCALL) {
    syscall_table[num] = handler;
  }
}

// ── Dispatcher (called from syscall_entry.asm) ──────────────────────────────
void syscall_dispatcher(struct syscall_regs *regs) {
  if (regs->rax >= MAX_SYSCALL || !syscall_table[regs->rax]) {
    klog_puts("\n[SYSCALL] Unimplemented syscall: ");
    klog_uint64(regs->rax);
    klog_puts("\n");
    regs->rax = (uint64_t)-1; // ENOSYS
    return;
  }

  syscall_handler_t handler = syscall_table[regs->rax];
  regs->rax =
      handler(regs->rdi, regs->rsi, regs->rdx, regs->r10, regs->r8, regs->r9);
}

// ── Core initialization ────────────────────────────────────────────────────
void syscall_init(void) {
  // Register all syscall subsystems
  syscall_register_io();
  syscall_register_process();
  syscall_register_mm();
  syscall_register_arch();

  // 1. Enable System Call Extensions and No-Execute
  uint64_t efer = rdmsr(IA32_EFER);
  efer |= IA32_EFER_SCE | (1ULL << 11); // Set NXE
  wrmsr(IA32_EFER, efer);

  // 2. Configure STAR:
  // KCode=0x08, KData=0x10, UData=0x18 | 3, UCode=0x20 | 3
  uint64_t star = ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32);
  wrmsr(IA32_STAR, star);

  // 3. Configure LSTAR with the syscall entry point
  wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);

  // 4. Configure FMASK (Mask interrupts IF=0x200 while in syscall)
  wrmsr(IA32_FMASK, 0x200);

  klog_puts("[OK] Syscall Infrastructure (MSRs) initialized.\n");
}
