#ifndef SCHED_H
#define SCHED_H

#include "../cpu/isr.h"
#include "../fs/vfs.h"
#include "../mm/vma.h"
#include <stddef.h>
#include <stdint.h>

#define MAX_FDS 32

typedef enum {
  THREAD_RUNNING,
  THREAD_READY,
  THREAD_BLOCKED,
  THREAD_SLEEPING,
  THREAD_DEAD,
  THREAD_ZOMBIE
} thread_state_t;

// Information saved on context switch.
// We push callee-saved registers manually in switch.asm.
struct context {
  uint64_t r15;
  uint64_t r14;
  uint64_t r13;
  uint64_t r12;
  uint64_t rbx;
  uint64_t rbp;
  uint64_t ret_addr; // RIP (pushed automatically by call)
} __attribute__((packed));

struct thread {
  uint64_t rsp; // Must be first field (offset 0) for optimal assembly
  uint32_t tid;
  uint64_t stack_base;
  uint64_t stack_size;
  thread_state_t state;
  uint64_t wakeup_ticks;
  vfs_node_t *fds[MAX_FDS];
  uint64_t fd_offsets[MAX_FDS]; // Track seek offset per file descriptor
  uint64_t cr3;                 // Per-process page table (0 = inherited/kernel)
  bool is_forked_child;         // True for forked children (affects sys_exit)
  bool is_idle;                 // True for idle thread (cannot be terminated)
  void *fork_ctx;               // Saved register state for child entry
  struct thread *parent;        // Pointer to parent thread (for wait4)
  int exit_status;              // Status code when exiting (for wait4)
  uint64_t *tid_address;        // Pointer to user-space TID for set_tid_address
  struct thread *global_next;   // Used to link all threads together
  struct thread *next;          // Used for runqueue / blocked queue
  char cwd_path[256];           // Current working directory
  struct vma_list vmas;         // Virtual memory areas for this process
  uint64_t fs_base;             // User FS_BASE (TLS) — inherited across fork
};

void sched_init(void);

struct cpu_info;
struct thread *sched_create_kernel_thread(void (*entry_point)(void),
                                          struct cpu_info *explicit_cpu);

void sched_tick(struct registers *regs);
void sched_yield(void);

// Returns the current thread *for the CPU currently executing this code*
struct thread *sched_get_current(void);

// Load balancing / dispatching
void sched_enqueue_thread(struct thread *t, struct cpu_info *explicit_cpu);

// Task management for shell
void sched_print_tasks(void);
bool sched_terminate_thread(uint32_t tid);

// Userspace Management
#include <stdbool.h>
#include "elf.h"
bool elf_load(const char *path, uint64_t *pml4, elf_info_t *out_info);
// Linux-style stack: path string near top, then auxv/envp/argv/argc; RSP points
// at argc.
uint64_t process_build_initial_stack(uint64_t stack_top, const char *path,
                                     const char **argv, const char **envp,
                                     const elf_info_t *elf_info);
bool process_exec_argv(const char **argv);

#define ASCENTOS_USER_STACK_TOP 0x00007FFFF0000000ULL

#endif
