// ── Process Syscalls: fork, getpid, exit ────────────────────────────────────
#include "syscall.h"
#include "../console/klog.h"
#include "../cpu/gdt.h"
#include "../cpu/msr.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../mm/vma.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"
#include <stdint.h>

// Defined in process.c — the saved kernel context from before entering Ring 3
typedef uint64_t kernel_jmp_buf[8];
extern kernel_jmp_buf process_return_ctx;
extern void kernel_longjmp(kernel_jmp_buf buf, int val) __attribute__((noreturn));

// Assembly trampoline that restores user registers and sysrets to Ring 3
extern void fork_return_to_userspace(struct syscall_regs *regs)
    __attribute__((noreturn));

// MSR for TLS base registers
#define IA32_KERNEL_GS_BASE 0xC0000102
#define IA32_FS_BASE        0xC0000100

// ── exit / exit_group (shared) ─────────────────────────────────────────────
static void process_do_exit(uint64_t status) __attribute__((noreturn));
static void process_do_exit(uint64_t status) {
  struct thread *current = sched_get_current();

  klog_puts("\n[SYSCALL] Process exited with status: ");
  klog_uint64(status);
  klog_puts("\n");

  // If tid_address was set via set_tid_address, clear it (set to 0) to signal exit
  if (current && current->tid_address) {
    *current->tid_address = 0;
  }

  if (current && current->is_forked_child) {
    __asm__ volatile("mov $0x10, %%ax\n"
                     "mov %%ax, %%ds\n"
                     "mov %%ax, %%es\n" ::: "eax");
    __asm__ volatile("sti");

    current->exit_status = (int)status;
    current->state = THREAD_ZOMBIE;

    if (current->parent && current->parent->state == THREAD_BLOCKED) {
      current->parent->state = THREAD_READY;
    }

    while (1) {
      sched_yield();
    }
  }

  kernel_longjmp(process_return_ctx, 1);
}

static uint64_t __attribute__((noreturn))
sys_exit(uint64_t status, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
         uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_do_exit(status);
}

static uint64_t __attribute__((noreturn))
sys_exit_group(uint64_t status, uint64_t a1, uint64_t a2, uint64_t a3,
               uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  process_do_exit(status);
}

static uint64_t sys_set_tid_address(uint64_t tidptr, uint64_t a1, uint64_t a2,
                                    uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct thread *current = sched_get_current();
  if (!current)
    return 0;
  
  // Store the pointer to the user-space TID variable
  // This is used by musl/glibc for thread exit notification
  current->tid_address = (uint64_t *)tidptr;
  
  // Return the current thread's ID
  return current->tid;
}

// ── sys_getpid ──────────────────────────────────────────────────────────────
static uint64_t sys_getpid(uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

  struct thread *current = sched_get_current();
  if (current) {
    return current->tid;
  }
  return 0;
}

// ── sys_wait4 ───────────────────────────────────────────────────────────────
extern struct thread *global_thread_list; // From sched.c

static uint64_t sys_wait4(uint64_t pid, uint64_t wstatus_ptr, uint64_t options,
                          uint64_t rusage, uint64_t a4, uint64_t a5) {
  (void)options; (void)rusage; (void)a4; (void)a5;
  struct thread *current = sched_get_current();
  if (!current) return (uint64_t)-10; // ECHILD

  int target_pid = (int)pid;

  while (1) {
      bool has_children = false;
      struct thread *zombie = NULL;

      // Scan global list for children
      struct thread *t = global_thread_list;
      
      while (t) {
          if (t->parent == current) {
              if (target_pid <= 0 || t->tid == (uint32_t)target_pid) {
                  has_children = true;
                  if (t->state == THREAD_ZOMBIE) {
                      zombie = t;
                      break;
                  }
              }
          }
          t = t->global_next;
      }

      if (zombie) {
          klog_puts("[WAIT4] Reaping zombie PID ");
          klog_uint64(zombie->tid);
          klog_puts("\n");

          if (wstatus_ptr != 0) {
              int *wstatus = (int *)wstatus_ptr;
              *wstatus = (zombie->exit_status & 0xFF) << 8;
          }
          uint32_t reaped_pid = zombie->tid;
          
          // Fully dead now
          zombie->state = THREAD_DEAD; 
          zombie->parent = NULL; // Detach
          
          return reaped_pid;
      }

      if (!has_children) {
          return (uint64_t)-10; // ECHILD
      }

      // Block and wait for a child to exit
      current->state = THREAD_BLOCKED;
      sched_yield();
  }
}

// ── sys_execve ──────────────────────────────────────────────────────────────
static uint64_t sys_execve(struct syscall_regs *regs) {
  const char **user_argv = (const char **)regs->rsi;
  const char **user_envp = (const char **)regs->rdx;
  const char *user_path = user_argv && user_argv[0] ? user_argv[0] : (const char *)regs->rdi;
  
  if (!user_path) return (uint64_t)-14; // EFAULT

  // Copy path to kernel heap because we're about to switch CR3!
  size_t path_len = strlen(user_path);
  char *path = kmalloc(path_len + 1);
  if (!path) return (uint64_t)-12; // ENOMEM
  memcpy(path, user_path, path_len + 1);

  klog_puts("[EXECVE] Executing: ");
  klog_puts(path);
  klog_puts("\n");

  uint64_t *new_pml4 = vmm_create_pml4();
  if (!new_pml4) {
      kfree(path);
      return (uint64_t)-12; // ENOMEM
  }

  // EARLY CR3 SWITCH:
  // We must switch the hardware CR3 to the new address space BEFORE we call elf_load,
  // so that when elf_load mmaps and writes the binary from disk, it writes to the 
  // correctly allocated physical pages, not the parent's address space!
  struct thread *current = sched_get_current();
  uint64_t old_cr3 = current->cr3;
  
  current->cr3 = (uint64_t)new_pml4;
  __asm__ volatile("mov %0, %%cr3" :: "r"(current->cr3) : "memory");

  uint64_t entry_point = 0;
  if (!elf_load(path, new_pml4, &entry_point)) {
      klog_puts("[EXECVE] Failed to load ELF\n");
      // Revert CR3
      current->cr3 = old_cr3;
      __asm__ volatile("mov %0, %%cr3" :: "r"(current->cr3) : "memory");
      kfree(path);
      return (uint64_t)-8; // ENOEXEC
  }

  // We are now safely loaded into the new address space!
  uint64_t user_rsp = process_build_initial_stack(ASCENTOS_USER_STACK_TOP, NULL, user_argv, user_envp);

  klog_puts("[EXECVE] Success, returning to user space at ");
  klog_uint64(entry_point);
  klog_puts("\n");

  // Free the kernel heap copy of the path since it's now in user space
  kfree(path);

  // Reset TLS Bases for this process
  wrmsr(IA32_KERNEL_GS_BASE, 0);
  wrmsr(IA32_FS_BASE, 0);

  // Set the return registers for the syscall exit handler to jump to!
  regs->rip = entry_point;
  regs->rsp = user_rsp;

  // We are heavily relying on the fact that `regs` pointer is on the KERNEL stack
  // which is in the higher half and fully mapped identically in `new_pml4`.
  return 0;
}

// ── Fork child kernel thread entry point ────────────────────────────────────
// This function runs as a kernel thread.  When scheduled, it switches to the
// child's page table and sysrets to the user-space address where the parent
// called fork().  RAX will be 0 (child return value).
static void fork_child_entry(void) {
  struct thread *self = sched_get_current();
  struct syscall_regs *child_regs = (struct syscall_regs *)self->fork_ctx;

  klog_puts("[FORK] Child thread ");
  klog_uint64(self->tid);
  klog_puts(" entering userspace\n");

  // Switch to the child's cloned address space
  __asm__ volatile("mov %0, %%cr3" :: "r"(self->cr3) : "memory");

  // Set TSS rsp0 so interrupts and syscalls from Ring 3 use this CPU's
  // kernel stack.
  tss_set_rsp0(cpu_get_current()->stack_top);

  // Reset user TLS bases to 0 (child starts with clean TLS)
  wrmsr(IA32_KERNEL_GS_BASE, 0);
  wrmsr(IA32_FS_BASE, 0);

  // Jump to userspace — this never returns
  fork_return_to_userspace(child_regs);
}

// ── sys_fork (raw handler — receives full register frame) ───────────────────
static uint64_t sys_fork(struct syscall_regs *regs) {
  klog_puts("[FORK] Fork requested\n");

  // 1. Get current parent state
  uint64_t *parent_pml4_phys = vmm_get_active_pml4();
  struct thread *parent = sched_get_current();

  // 2. Clone the user address space with VMA awareness
  //    Shared mappings share physical pages, private mappings get copied
  uint64_t child_cr3 = vmm_clone_user_mappings_vma(parent_pml4_phys, 
                                                    parent ? &parent->vmas : NULL);
  if (child_cr3 == 0) {
    klog_puts("[FORK] Failed: could not clone address space\n");
    return (uint64_t)(-12); // -ENOMEM
  }

  klog_puts("[FORK] Address space cloned, child CR3: ");
  klog_uint64(child_cr3);
  klog_puts("\n");

  // 3. Allocate and populate the child's saved register state.
  //    RAX = 0 so the child sees fork() returning 0.
  struct syscall_regs *child_regs = kmalloc(sizeof(struct syscall_regs));
  if (!child_regs) {
    klog_puts("[FORK] Failed: OOM for child regs\n");
    return (uint64_t)(-12);
  }
  memcpy(child_regs, regs, sizeof(struct syscall_regs));
  child_regs->rax = 0; // Child return value

  // 4. Create a kernel thread for the child process.
  //    sched_create_kernel_thread enqueues it on a CPU's run queue.
  //    We FORCE it to the current CPU for now to ensure visibility and scheduling.
  struct thread *child = sched_create_kernel_thread(fork_child_entry, cpu_get_current());
  if (!child) {
    kfree(child_regs);
    klog_puts("[FORK] Failed: could not create child thread\n");
    return (uint64_t)(-12);
  }

  // 5. Configure child thread
  child->cr3 = child_cr3;
  child->is_forked_child = true;
  child->fork_ctx = child_regs;

  // 6. Copy file descriptors from parent to child
  if (parent) {
    for (int i = 0; i < MAX_FDS; i++) {
      child->fds[i] = parent->fds[i];
      child->fd_offsets[i] = parent->fd_offsets[i];
    }
    // 7. Copy VMA list and cwd from parent to child
    vma_list_clone(&child->vmas, &parent->vmas);
    memcpy(child->cwd_path, parent->cwd_path, sizeof(child->cwd_path));
  }

  klog_puts("[FORK] Child created with PID ");
  klog_uint64(child->tid);
  klog_puts("\n");

  // 8. Return child PID to parent
  return child->tid;
}

// ── Registration ────────────────────────────────────────────────────────────
void syscall_register_process(void) {
  syscall_register(SYS_EXIT, sys_exit);
  syscall_register(SYS_EXIT_GROUP, sys_exit_group);
  syscall_register(SYS_SET_TID_ADDRESS, sys_set_tid_address);
  syscall_register(SYS_GETPID, sys_getpid);
  syscall_register(SYS_WAIT4, sys_wait4);
  syscall_register_raw(SYS_FORK, sys_fork);
  syscall_register_raw(SYS_EXECVE, sys_execve);
}
