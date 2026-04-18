// ── Process Syscalls: fork, getpid, exit ────────────────────────────────────
#include "../console/klog.h"
#include "../cpu/gdt.h"
#include "../cpu/msr.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vma.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include "../smp/cpu.h"
#include "syscall.h"
#include <stdint.h>

extern void mm_reset_mmap_state(struct thread *t);
void vma_list_init(struct vma_list *list);

// ── Path Normalization ──────────────────────────────────────────────────────
static void path_normalize(const char *base, const char *rel, char *out) {
  char temp[512] = {0};

  // If relative path starts with '/', it replaces base entirely
  if (rel[0] == '/') {
    strcpy(temp, rel);
  } else {
    strcpy(temp, base);
    if (temp[strlen(temp) - 1] != '/')
      strcat(temp, "/");
    strcat(temp, rel);
  }

  // Collapse elements
  char *collapsed[128];
  int count = 0;

  char *p = temp;
  while (*p) {
    while (*p == '/')
      *p++ = '\0';
    if (!*p)
      break;

    char *elem = p;
    while (*p && *p != '/') {
      p++;
    }
    if (*p == '/') {
      *p = '\0';
      p++;
    }

    if (strcmp(elem, ".") == 0) {
      continue;
    } else if (strcmp(elem, "..") == 0) {
      if (count > 0)
        count--;
    } else {
      if (count < 128) {
        collapsed[count++] = elem;
      }
    }
  }

  // Rebuild out path
  out[0] = '\0';
  for (int i = 0; i < count; i++) {
    strcat(out, "/");
    strcat(out, collapsed[i]);
  }
  if (count == 0)
    strcpy(out, "/");
}

// Defined in process.c — the saved kernel context from before entering Ring 3
typedef uint64_t kernel_jmp_buf[8];
extern kernel_jmp_buf process_return_ctx;
extern void kernel_longjmp(kernel_jmp_buf buf, int val)
    __attribute__((noreturn));

// Assembly trampoline that restores user registers and sysrets to Ring 3
extern void fork_return_to_userspace(struct syscall_regs *regs)
    __attribute__((noreturn));

// MSR for TLS base registers
#define IA32_KERNEL_GS_BASE 0xC0000102
#define IA32_FS_BASE 0xC0000100

// ── exit / exit_group (shared) ─────────────────────────────────────────────
void process_do_exit(uint64_t status) __attribute__((noreturn));
void process_do_exit(uint64_t status) {
  struct thread *current = sched_get_current();
  if (current) {
    sched_reparent_children(current);
  }

  klog_puts("\n[SYSCALL] Process exited with status: ");
  klog_uint64(status);
  klog_puts("\n");

  // If tid_address was set via set_tid_address, clear it (set to 0) to signal
  // exit
  if (current && current->tid_address) {
    *current->tid_address = 0;
  }

  if (current && current->is_forked_child) {
    // Ensure kernel data segments are loaded
    __asm__ volatile("mov $0x10, %%ax\n"
                     "mov %%ax, %%ds\n"
                     "mov %%ax, %%es\n" ::
                         : "eax");
    __asm__ volatile("sti");

    // Pre-free the user address space NOW, while we're still on our own
    // kernel stack. This avoids the parent having to do heavy page-table
    // walking during reap (which risks corrupting the parent's execution
    // context). Switch to kernel CR3 first so we don't pull the rug from
    // under ourselves.
    if (current->cr3) {
      extern uint64_t *vmm_get_active_pml4(void);
      // Read the kernel CR3 from our CPU
      struct cpu_info *cpu = cpu_get_current();
      uint64_t saved_cr3 = current->cr3;
      // Switch to kernel address space
      __asm__ volatile("mov %0, %%cr3" ::"r"(cpu->kernel_cr3) : "memory");
      current->cr3 = 0;
      // Now safely free the child's user pages
      vmm_free_user_pages(saved_cr3);
    }

    current->exit_status = (int)status;
    current->state = THREAD_ZOMBIE;

    if (current->parent && current->parent->state == THREAD_BLOCKED) {
      current->parent->state = THREAD_READY;
    }

    // Spin-yield forever; the parent will reap us.
    while (1) {
      sched_yield();
    }
  }

  kernel_longjmp(process_return_ctx, 1);
}

static uint64_t __attribute__((noreturn)) sys_exit(uint64_t status, uint64_t a1,
                                                   uint64_t a2, uint64_t a3,
                                                   uint64_t a4, uint64_t a5) {
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
static uint64_t sys_getpid(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5) {
  (void)a0;
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;

  struct thread *current = sched_get_current();
  if (current) {
    return current->tid;
  }
  return 0;
}
// ── sys_getcwd ──────────────────────────────────────────────────────────────
static uint64_t sys_getcwd(uint64_t buf_ptr, uint64_t size, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  char *buf = (char *)buf_ptr;
  if (!buf)
    return (uint64_t)-14; // EFAULT

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  size_t len = strlen(current->cwd_path) + 1;
  if (size < len)
    return (uint64_t)-34; // ERANGE

  memcpy(buf, current->cwd_path, len);
  return len; // Linux getcwd syscall returns length of copied bytes
}

// ── sys_chdir ───────────────────────────────────────────────────────────────
static uint64_t sys_chdir(uint64_t path_ptr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  const char *path = (const char *)path_ptr;
  if (!path)
    return (uint64_t)-14; // EFAULT

  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-1;

  char new_path[256];
  path_normalize(current->cwd_path, path, new_path);

  // Validate the directory exists!
  // Note: we want an absolute lookup here
  vfs_node_t *node = vfs_resolve_path_at(fs_root, new_path);
  if (!node)
    return (uint64_t)-2; // ENOENT

  if ((node->flags & 0xFF) != FS_DIRECTORY)
    return (uint64_t)-20; // ENOTDIR

  // If validation passes, update thread
  strncpy(current->cwd_path, new_path, 255);
  current->cwd_path[255] = '\0';

  return 0; // Success
}
extern spinlock_t tid_lock;

#define WNOHANG 1

static uint64_t sys_wait4(uint64_t pid, uint64_t wstatus_ptr, uint64_t options,
                          uint64_t rusage, uint64_t a4, uint64_t a5) {
  (void)rusage;
  (void)a4;
  (void)a5;
  struct thread *current = sched_get_current();
  if (!current)
    return (uint64_t)-10; // ECHILD

  int target_pid = (int)pid;

  while (1) {
    bool has_matching_children = false;
    struct thread *zombie = NULL;

    spinlock_acquire(&tid_lock);
    struct thread *t = current->children;
    while (t) {
      bool matches = false;
      if (target_pid == -1) {
        matches = true;
      } else if (target_pid > 0) {
        if (t->tid == (uint32_t)target_pid)
          matches = true;
      } else if (target_pid == 0) {
        if (t->pgid == current->pgid)
          matches = true;
      } else { // target_pid < -1
        if (t->pgid == (uint32_t)(-target_pid))
          matches = true;
      }

      if (matches) {
        has_matching_children = true;
        if (t->state == THREAD_ZOMBIE) {
          zombie = t;
          break;
        }
      }
      t = t->sibling_next;
    }
    spinlock_release(&tid_lock);

    if (zombie) {
      if (wstatus_ptr != 0) {
        int *wstatus = (int *)wstatus_ptr;
        *wstatus = (zombie->exit_status & 0xFF) << 8;
      }
      uint32_t reaped_pid = zombie->tid;

      // Fully reap the zombie (remove from runqueue, free resources)
      sched_reap_thread(zombie);

      return (uint64_t)reaped_pid;
    }

    if (!has_matching_children) {
      return (uint64_t)-10; // ECHILD
    }

    // WNOHANG: return 0 immediately if no zombie found
    if (options & WNOHANG) {
      return 0;
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
  const char *user_path = (const char *)regs->rdi;

  if (!user_path)
    return (uint64_t)-14; // EFAULT

  // 1. Copy everything to kernel memory BEFORE switching CR3
  // We need to copy the path, the argv array, and the envp array
  size_t path_len = strlen(user_path);
  char *path = kmalloc(path_len + 1);
  if (!path)
    return (uint64_t)-12;
  memcpy(path, user_path, path_len + 1);

  int argc = 0;
  if (user_argv)
    while (user_argv[argc])
      argc++;
  char **k_argv = kmalloc((argc + 1) * sizeof(char *));
  for (int i = 0; i < argc; i++) {
    size_t len = strlen(user_argv[i]);
    k_argv[i] = kmalloc(len + 1);
    memcpy(k_argv[i], user_argv[i], len + 1);
  }
  k_argv[argc] = NULL;

  int envc = 0;
  if (user_envp)
    while (user_envp[envc])
      envc++;
  char **k_envp = kmalloc((envc + 1) * sizeof(char *));
  for (int i = 0; i < envc; i++) {
    size_t len = strlen(user_envp[i]);
    k_envp[i] = kmalloc(len + 1);
    memcpy(k_envp[i], user_envp[i], len + 1);
  }
  k_envp[envc] = NULL;

  klog_puts("[EXECVE] Executing: ");
  klog_puts(path);
  klog_puts("\n");

  uint64_t *new_pml4 = vmm_create_pml4();
  if (!new_pml4) {
    // Cleanup skipped for brevity in this first pass, but ideally we'd kfree
    // everything
    kfree(path);
    return (uint64_t)-12;
  }

  // EARLY CR3 SWITCH:
  struct thread *current = sched_get_current();
  uint64_t old_cr3 = current->cr3;

  current->cr3 = (uint64_t)new_pml4;
  __asm__ volatile("mov %0, %%cr3" ::"r"(current->cr3) : "memory");

  // Reset memory management state for the new program
  vma_list_init(&current->vmas);
  mm_reset_mmap_state(current);
  current->fs_base = 0;

  elf_info_t elf_info = {0};
  if (!elf_load(path, new_pml4, &elf_info)) {
    klog_puts("[EXECVE] Failed to load ELF\n");
    // Revert CR3
    current->cr3 = old_cr3;
    __asm__ volatile("mov %0, %%cr3" ::"r"(current->cr3) : "memory");
    kfree(path);
    return (uint64_t)-8; // ENOEXEC
  }

  // We are now safely loaded into the new address space!
  uint64_t user_rsp = process_build_initial_stack(
      ASCENTOS_USER_STACK_TOP, path, (const char **)k_argv,
      (const char **)k_envp, &elf_info);

  klog_puts("[EXECVE] Success, returning to user space at ");
  klog_uint64(elf_info.entry);
  klog_puts("\n");

  // Free the old address space (from fork) now that the new one is loaded.
  // We've already switched CR3, so this is safe.
  if (old_cr3 != 0) {
    vmm_free_user_pages(old_cr3);
  }

  // Cleanup kernel-side copies
  for (int i = 0; i < argc; i++)
    kfree(k_argv[i]);
  kfree(k_argv);
  for (int i = 0; i < envc; i++)
    kfree(k_envp[i]);
  kfree(k_envp);
  kfree(path);

  // Reset TLS Bases for this process
  wrmsr(IA32_KERNEL_GS_BASE, 0);
  wrmsr(IA32_FS_BASE, 0);

  // Set the return registers for the syscall exit handler to jump to!
  regs->rip = elf_info.entry;
  regs->rsp = user_rsp;

  // We are heavily relying on the fact that `regs` pointer is on the KERNEL
  // stack which is in the higher half and fully mapped identically in
  // `new_pml4`.
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
  __asm__ volatile("mov %0, %%cr3" ::"r"(self->cr3) : "memory");

  // Set TSS rsp0 so interrupts and syscalls from Ring 3 use this CPU's
  // kernel stack.
  tss_set_rsp0(cpu_get_current()->stack_top);

  // Restore user TLS bases — child inherits parent's FS_BASE (musl needs TLS)
  wrmsr(IA32_KERNEL_GS_BASE, 0);
  wrmsr(IA32_FS_BASE, self->fs_base);

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
  uint64_t child_cr3 = vmm_clone_user_mappings_vma(
      parent_pml4_phys, parent ? &parent->vmas : NULL);
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
  //    We FORCE it to the current CPU for now to ensure visibility and
  //    scheduling.
  struct thread *child =
      sched_create_kernel_thread(fork_child_entry, cpu_get_current());
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
    // 7. Copy VMA list, cwd, and TLS base from parent to child
    vma_list_clone(&child->vmas, &parent->vmas);
    memcpy(child->cwd_path, parent->cwd_path, sizeof(child->cwd_path));
    child->fs_base = parent->fs_base;
    child->umask = parent->umask;
    child->uid = parent->uid;
    child->gid = parent->gid;
    child->euid = parent->euid;
    child->egid = parent->egid;
    child->suid = parent->suid;
    child->sgid = parent->sgid;
  }

  klog_puts("[FORK] Child created with PID ");
  klog_uint64(child->tid);
  klog_puts("\n");

  // 8. Return child PID to parent
  return child->tid;
}

// ── sys_clone ───────────────────────────────────────────────────────────────
static uint64_t sys_clone(struct syscall_regs *regs) {
  // For now, treat clone as fork (ignores threading flags).
  // Musl uses clone() to implement fork(), so this is essential.
  return sys_fork(regs);
}

// ── sys_uname ─────────────────────────────────────────────────────────────
struct utsname {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
  char domainname[65];
};

static uint64_t sys_uname(uint64_t buf_ptr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
  (void)a1;
  (void)a2;
  (void)a3;
  (void)a4;
  (void)a5;
  struct utsname *buf = (struct utsname *)buf_ptr;
  if (!buf)
    return (uint64_t)-14; // EFAULT

  strcpy(buf->sysname, "AscentOS");
  strcpy(buf->nodename, "ascentos");
  strcpy(buf->release, "0.1");
  strcpy(buf->version, "1.0");
  strcpy(buf->machine, "x86_64");
  strcpy(buf->domainname, "");

  strcpy(buf->domainname, "");

  return 0;
}

static uint64_t sys_prctl(uint64_t option, uint64_t arg2, uint64_t arg3,
                          uint64_t arg4, uint64_t arg5, uint64_t a5) {
  (void)option;
  (void)arg2;
  (void)arg3;
  (void)arg4;
  (void)arg5;
  (void)a5;
  return 0; // Success stub
}

// ── sys_umask ───────────────────────────────────────────────────────────────
static uint64_t sys_umask(struct syscall_regs *regs) {
  uint32_t mask = (uint32_t)regs->rdi;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;

  uint32_t old_mask = t->umask;
  t->umask = mask & 0777;
  return old_mask;
}

// ── sys_getuid ──────────────────────────────────────────────────────────────
static uint64_t sys_getuid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->uid;
}

// ── sys_getgid ──────────────────────────────────────────────────────────────
static uint64_t sys_getgid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->gid;
}

// ── sys_geteuid ─────────────────────────────────────────────────────────────
static uint64_t sys_geteuid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->euid;
}

// ── sys_getegid ─────────────────────────────────────────────────────────────
static uint64_t sys_getegid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->egid;
}

// ── sys_getresuid ───────────────────────────────────────────────────────────
static uint64_t sys_getresuid(struct syscall_regs *regs) {
  uint32_t *ruid_ptr = (uint32_t *)regs->rdi;
  uint32_t *euid_ptr = (uint32_t *)regs->rsi;
  uint32_t *suid_ptr = (uint32_t *)regs->rdx;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  if (ruid_ptr)
    *ruid_ptr = t->uid;
  if (euid_ptr)
    *euid_ptr = t->euid;
  if (suid_ptr)
    *suid_ptr = t->suid;

  return 0;
}

// ── sys_getresgid ───────────────────────────────────────────────────────────
static uint64_t sys_getresgid(struct syscall_regs *regs) {
  uint32_t *rgid_ptr = (uint32_t *)regs->rdi;
  uint32_t *egid_ptr = (uint32_t *)regs->rsi;
  uint32_t *sgid_ptr = (uint32_t *)regs->rdx;

  struct thread *t = sched_get_current();
  if (!t)
    return (uint64_t)-1;

  if (rgid_ptr)
    *rgid_ptr = t->gid;
  if (egid_ptr)
    *egid_ptr = t->egid;
  if (sgid_ptr)
    *sgid_ptr = t->sgid;

  return 0;
}

// ── sys_getppid ─────────────────────────────────────────────────────────────
static uint64_t sys_getppid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  if (t->parent)
    return t->parent->tid;
  return 0;
}

// ── sys_setpgid ─────────────────────────────────────────────────────────────
static uint64_t sys_setpgid(struct syscall_regs *regs) {
  uint32_t pid = (uint32_t)regs->rdi;
  uint32_t pgid = (uint32_t)regs->rsi;

  struct thread *current = sched_get_current();
  struct thread *target = NULL;

  if (pid == 0) {
    target = current;
  } else {
    // Only allow setting PGID for self or children
    if (current->tid == pid) {
      target = current;
    } else {
      spinlock_acquire(&tid_lock);
      struct thread *c = current->children;
      while (c) {
        if (c->tid == pid) {
          target = c;
          break;
        }
        c = c->sibling_next;
      }
      spinlock_release(&tid_lock);
    }
  }

  if (!target)
    return (uint64_t)-3; // ESRCH

  if (pgid == 0)
    pgid = target->tid;
  target->pgid = pgid;
  return 0;
}

// ── sys_getpgrp ─────────────────────────────────────────────────────────────
static uint64_t sys_getpgrp(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->pgid;
}

// ── sys_setsid (stub) ──────────────────────────────────────────────────────
static uint64_t sys_setsid(struct syscall_regs *regs) {
  (void)regs;
  struct thread *t = sched_get_current();
  if (!t)
    return 0;
  return t->tid; // Return own PID as new session ID
}

// ── Registration ────────────────────────────────────────────────────────────
void syscall_register_process(void) {
  syscall_register(SYS_EXIT, sys_exit);
  syscall_register(SYS_EXIT_GROUP, sys_exit_group);
  syscall_register(SYS_SET_TID_ADDRESS, sys_set_tid_address);
  syscall_register(SYS_GETPID, sys_getpid);
  syscall_register(SYS_WAIT4, sys_wait4);
  syscall_register(SYS_UNAME, sys_uname);
  syscall_register(SYS_GETCWD, sys_getcwd);
  syscall_register(SYS_CHDIR, sys_chdir);
  syscall_register(SYS_PRCTL, sys_prctl);
  syscall_register_raw(SYS_FORK, sys_fork);
  syscall_register_raw(SYS_CLONE, sys_clone);
  syscall_register_raw(SYS_EXECVE, sys_execve);
  syscall_register_raw(SYS_UMASK, sys_umask);
  syscall_register_raw(SYS_GETUID, sys_getuid);
  syscall_register_raw(SYS_GETGID, sys_getgid);
  syscall_register_raw(SYS_GETEUID, sys_geteuid);
  syscall_register_raw(SYS_GETEGID, sys_getegid);
  syscall_register_raw(SYS_GETRESUID, sys_getresuid);
  syscall_register_raw(SYS_GETRESGID, sys_getresgid);
  syscall_register_raw(SYS_GETPPID, sys_getppid);
  syscall_register_raw(SYS_SETPGID, sys_setpgid);
  syscall_register_raw(SYS_GETPGRP, sys_getpgrp);
  syscall_register_raw(SYS_SETSID, sys_setsid);
}
