// ── Signal Syscalls: rt_sigaction, rt_sigprocmask ───────────────────────────
#include "syscall.h"
#include "../console/klog.h"
#include "../sched/sched.h"
#include <stdint.h>

// Kernel ABI for x86_64 rt_sigaction uses a single 64-bit mask word when
// sigsetsize == sizeof(uint64_t) (what musl passes on this target).
struct k_sigaction {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;
};

// Signal handler table: one per signal (64 signals)
typedef struct {
    uint64_t handler;
    uint64_t flags;
} signal_handler_t;

static signal_handler_t signal_handlers[64] = {0};

// ── rt_sigaction: Set or get signal action ──────────────────────────────────
static uint64_t sys_rt_sigaction(uint64_t signum, uint64_t act_ptr,
                                  uint64_t oldact_ptr, uint64_t sigsetsize,
                                  uint64_t a4, uint64_t a5) {
  (void)a4;
  (void)a5;
  
  // Validate signal number
  if (signum >= 64 || signum < 1) {
    return (uint64_t)-22; // EINVAL
  }
  
  // Default sigsetsize should be 8 (sizeof(uint64_t))
  if (sigsetsize != 8) {
    return (uint64_t)-22; // EINVAL
  }
  
  uint64_t idx = signum - 1;

  // If oldact_ptr is provided, store the old signal action
  if (oldact_ptr) {
    struct k_sigaction *old = (struct k_sigaction *)oldact_ptr;
    if (!old) return (uint64_t)-14; // EFAULT

    old->sa_handler = signal_handlers[idx].handler;
    old->sa_flags = signal_handlers[idx].flags;
    old->sa_restorer = 0;
    old->sa_mask = 0;
  }
  
  // If act_ptr is provided, set the new signal action
  if (act_ptr) {
    struct k_sigaction *new = (struct k_sigaction *)act_ptr;
    if (!new) return (uint64_t)-14; // EFAULT
    
    signal_handlers[idx].handler = new->sa_handler;
    signal_handlers[idx].flags = new->sa_flags;
    
    // For now, we don't actually install signal handlers in the kernel
    // This is a stub that accepts the configuration but doesn't implement
    // actual signal delivery (that would require interrupt handling, etc.)
  }
  
  return 0; // Success
}

// ── rt_sigprocmask: Set or get signal mask ──────────────────────────────────
static uint64_t sys_rt_sigprocmask(uint64_t how, uint64_t set_ptr,
                                    uint64_t oldset_ptr, uint64_t sigsetsize,
                                    uint64_t a4, uint64_t a5) {
  (void)how;
  (void)set_ptr;
  (void)oldset_ptr;
  (void)sigsetsize;
  (void)a4;
  (void)a5;
  
  // Stub: Just return success
  // Full implementation would maintain per-thread signal masks
  return 0;
}

// Stub for tgkill - used by musl for thread cancellation
static uint64_t sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)tgid;
  (void)tid;
  (void)sig;
  (void)a3;
  (void)a4;
  (void)a5;
  // Return success - we don't implement signals yet
  return 0;
}

// Linux stack_t structure for sigaltstack
typedef struct {
  uint64_t ss_sp;    // Pointer to stack
  uint32_t ss_flags; // Flags
  uint32_t __pad;    // Padding
  uint64_t ss_size;  // Size of stack
} stack_t;

// Stub for sigaltstack - needed for Rust panic handling
static uint64_t sys_sigaltstack(uint64_t ss_ptr, uint64_t old_ss_ptr,
                                uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2; (void)a3; (void)a4; (void)a5;
  
  // If old_ss_ptr is provided, store the old alt stack info
  if (old_ss_ptr) {
    stack_t *old = (stack_t *)old_ss_ptr;
    old->ss_sp = 0;
    old->ss_flags = 0x00000002; // SS_DISABLE
    old->ss_size = 0;
  }
  
  // If ss_ptr is provided, set the new alt stack
  if (ss_ptr) {
    stack_t *ss = (stack_t *)ss_ptr;
    // Check for SS_DISABLE flag - if set, just disable alt stack
    if (ss->ss_flags & 0x00000002) {
      // SS_DISABLE - disable the alt stack, always succeeds
      return 0;
    }
    // Validate minimum size for enabled stack
    if (ss->ss_size < 8192) {
      return (uint64_t)-22; // EINVAL - too small
    }
    // Accept the stack configuration - we don't enforce it but pretend to
    // This allows Rust's std to proceed with its stack setup
  }
  
  return 0; // Success
}

// Old non-rt sigprocmask (syscall 186) - wrapper around rt_sigprocmask
static uint64_t sys_sigprocmask(uint64_t how, uint64_t set_ptr,
                                uint64_t oldset_ptr, uint64_t a3,
                                uint64_t a4, uint64_t a5) {
  (void)a3; (void)a4; (void)a5;
  // Just call the rt version with sigsetsize=8
  return sys_rt_sigprocmask(how, set_ptr, oldset_ptr, 8, 0, 0);
}

void syscall_register_signal(void) {
  syscall_register(SYS_RT_SIGACTION, sys_rt_sigaction);
  syscall_register(SYS_RT_SIGPROCMASK, sys_rt_sigprocmask);
  syscall_register(SYS_SIGPROCMASK, sys_sigprocmask);
  syscall_register(SYS_SIGALTSTACK, sys_sigaltstack);
  syscall_register(SYS_TGKILL, sys_tgkill);
}
