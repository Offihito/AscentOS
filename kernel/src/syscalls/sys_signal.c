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

void syscall_register_signal(void) {
  syscall_register(SYS_RT_SIGACTION, sys_rt_sigaction);
  syscall_register(SYS_RT_SIGPROCMASK, sys_rt_sigprocmask);
}
