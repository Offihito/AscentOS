#include "sched.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../cpu/idt.h"
#include "../cpu/msr.h"
#include "../drivers/timer/pit.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../smp/cpu.h"

static uint32_t next_tid = 1;
spinlock_t tid_lock = SPINLOCK_INIT;

// Deferred reaping structures
struct dead_thread_info {
  uint64_t stack_base;
  uint64_t thread_ptr;
  struct dead_thread_info *next;
};
struct dead_thread_info *dead_threads = NULL;
spinlock_t dead_threads_lock = SPINLOCK_INIT;

// Global list of all threads (for wait4)
struct thread *global_thread_list = NULL;

extern void switch_context(struct thread *old_t, struct thread *new_t);
extern void thread_stub(void); // Defined in switch.asm

void sched_init(void) {
  // We expect this to be called after cpu_init() which populates the CPU list
  uint32_t count = cpu_get_count();

  for (uint32_t i = 0; i < count; i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
      continue;

    // Create the idle thread for this specific CPU
    struct thread *idle_thread = kmalloc(sizeof(struct thread));
    memset(idle_thread, 0, sizeof(struct thread));
    idle_thread->cwd_path[0] = '/';
    // Assign a proper TID to the idle thread (don't use 0)
    spinlock_acquire(&tid_lock);
    idle_thread->tid = next_tid++;
    spinlock_release(&tid_lock);
    idle_thread->is_idle = true;
    idle_thread->pgid = idle_thread->tid;
    idle_thread->state = THREAD_RUNNING;
    idle_thread->next = idle_thread; // Circular queue

    idle_thread->stack_size = CPU_STACK_SIZE;
    idle_thread->stack_base = cpu->stack_top - CPU_STACK_SIZE;

    // Idle threads have no parent
    idle_thread->parent = NULL;
    idle_thread->global_next = global_thread_list;
    global_thread_list = idle_thread;

    cpu->current_thread = idle_thread;
    cpu->runqueue = idle_thread;
    spinlock_release(&cpu->queue_lock); // Initialize lock to 0
  }
}

static void thread_exit(void) {
  // Current thread finished execution. Mark dead and yield.
  __asm__ volatile("cli");
  struct cpu_info *cpu = cpu_get_current();
  if (cpu->current_thread) {
    cpu->current_thread->state = THREAD_DEAD;
  }
  // Infinite loop, yield will switch away
  while (1) {
    sched_yield();
  }
}

#define THREAD_STACK_SIZE 8192

void sched_enqueue_thread(struct thread *t, struct cpu_info *explicit_cpu) {
  struct cpu_info *target_cpu = explicit_cpu;

  if (!target_cpu) {
    // Basic Load Balancing: Round-Robin among available CPUs
    static uint32_t next_cpu = 0;

    spinlock_acquire(&tid_lock);
    uint32_t target_cpu_id = next_cpu;
    next_cpu = (next_cpu + 1) % cpu_get_count();
    spinlock_release(&tid_lock);

    target_cpu = cpu_get_info(target_cpu_id);
  }

  // Fallback just in case
  if (!target_cpu || target_cpu->status == CPU_STATUS_OFFLINE) {
    target_cpu = cpu_get_bsp();
  }

  __asm__ volatile("cli");
  spinlock_acquire(&target_cpu->queue_lock);

  if (!target_cpu->runqueue) {
    target_cpu->runqueue = t;
    t->next = t;
  } else {
    struct thread *tail = target_cpu->runqueue;
    while (tail->next != target_cpu->runqueue) {
      tail = tail->next;
    }
    tail->next = t;
    t->next = target_cpu->runqueue;
  }

  spinlock_release(&target_cpu->queue_lock);
  __asm__ volatile("sti");
}

struct thread *sched_create_kernel_thread(void (*entry)(void),
                                          struct cpu_info *explicit_cpu,
                                          bool enqueue) {
  // Allocate thread struct
  struct thread *t = kmalloc(sizeof(struct thread));
  if (!t)
    return NULL;

  memset(t, 0, sizeof(struct thread));
  t->cwd_path[0] = '/';
  t->umask = 0022;
  t->uid = t->gid = t->euid = t->egid = t->suid = t->sgid = 0;
  vma_list_init(&t->vmas);

  spinlock_acquire(&tid_lock);
  t->tid = next_tid++;
  t->global_next = global_thread_list;
  global_thread_list = t;

  // Set parent and link into hierarchy
  struct thread *current = sched_get_current();
  t->parent = current;
  if (current) {
    t->sibling_next = current->children;
    current->children = t;
    t->pgid = current->pgid; // Inherit PGID by default
  } else {
    t->pgid = t->tid; // Root threads have PGID = TID
  }
  spinlock_release(&tid_lock);

  t->state = THREAD_READY;
  t->stack_size = THREAD_STACK_SIZE;

  t->stack_base = (uint64_t)kmalloc(THREAD_STACK_SIZE);

  if (!t->stack_base) {
    kfree(t);
    return NULL;
  }

  uint64_t stack_top = t->stack_base + THREAD_STACK_SIZE;
  stack_top &= ~0xFULL; // Align stack

  // 1. Push the thread exit function (simulating a return address)
  stack_top -= 8;
  *(uint64_t *)stack_top = (uint64_t)thread_exit;

  // 2. Setup context frame
  stack_top -= sizeof(struct context);
  struct context *ctx = (struct context *)stack_top;
  memset(ctx, 0, sizeof(struct context));

  // Store entry function in r12 which thread_stub will call
  ctx->r12 = (uint64_t)entry;

  // `switch_context` does `ret`, popping this address
  ctx->ret_addr = (uint64_t)thread_stub;

  t->rsp = stack_top;

  // Initialize FPU state
  memset(t->fpu_state, 0, 512);

  // We can't easily call fninit here for the child buffer without clobbering 
  // current FPU state. However, we can just let switch_context handle it 
  // if we ensure it's zeroed (most CPUs treat zero as okay) or use a static init.
  static uint8_t fpu_init_done = 0;
  static uint8_t initial_fpu_state[512] __attribute__((aligned(16)));
  if (!fpu_init_done) {
    __asm__ volatile("fninit; fxsave64 %0" : "=m"(initial_fpu_state));
    fpu_init_done = 1;
  }
  memcpy(t->fpu_state, initial_fpu_state, 512);

  // Balance and add to a CPU's runqueue conditionally
  if (enqueue) {
    sched_enqueue_thread(t, explicit_cpu);
  }

  return t;
}

void sched_yield(void) {
  __asm__ volatile("cli");
  struct cpu_info *cpu = cpu_get_current();
  if (!cpu->current_thread) {
    __asm__ volatile("sti");
    return;
  }

  struct thread *prev = cpu->current_thread;

  // Use local pointers to prevent breaking if queue is modified
  spinlock_acquire(&cpu->queue_lock);
  struct thread *next_t = cpu->current_thread->next;
  struct thread *start_t = next_t; // Keep track of the starting thread

  // Find next ready/running thread
  while (next_t != cpu->current_thread) {
    if (next_t->state == THREAD_READY || next_t->state == THREAD_RUNNING) {
      break;
    }

    // Check for timeout on sleeping/blocked threads
    if ((next_t->state == THREAD_SLEEPING || next_t->state == THREAD_BLOCKED) &&
        next_t->wakeup_ticks != 0 && pit_get_ticks() >= next_t->wakeup_ticks) {
      next_t->state = THREAD_READY;
      next_t->wakeup_ticks = 0;
      break;
    }

    next_t = next_t->next;
    if (next_t == start_t) {
      // Broken loop gracefully, likely due to runqueue modifications
      break;
    }
  }

  if (next_t != cpu->current_thread &&
      (next_t->state == THREAD_READY || next_t->state == THREAD_RUNNING)) {
    if (prev->state == THREAD_RUNNING) {
      prev->state = THREAD_READY;
    }
    next_t->state = THREAD_RUNNING;
    cpu->current_thread = next_t;

    cpu->stack_top = (next_t->stack_base + next_t->stack_size) & ~0xFULL;
    extern void tss_set_rsp0(uint64_t rsp0);
    tss_set_rsp0(cpu->stack_top);

    uint64_t target_cr3 = next_t->cr3 ? next_t->cr3 : cpu->kernel_cr3;
    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    if (target_cr3 != current_cr3) {
      __asm__ volatile("mov %0, %%cr3" ::"r"(target_cr3) : "memory");
    }

    // Save current thread's TLS MSRs
    prev->fs_base = rdmsr(0xC0000100);

    // Restore next thread's TLS MSRs
    wrmsr(0xC0000100, next_t->fs_base);

    spinlock_release(&cpu->queue_lock);
    switch_context(prev, next_t);
  } else {
    spinlock_release(&cpu->queue_lock);
  }

  // Only enable here if we are returning normally
  __asm__ volatile("sti");
}

void sched_tick(struct registers *regs) {
  (void)regs;
  struct cpu_info *cpu = cpu_get_current();
  if (cpu->current_thread) {
    sched_yield();
  }
}

struct thread *sched_get_current(void) {
  return cpu_get_current()->current_thread;
}

void sched_print_tasks(void) {
  console_puts("TID  CPU  STATE       RSP\n");
  for (uint32_t i = 0; i < cpu_get_count(); i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
      continue;

    __asm__ volatile("cli");
    spinlock_acquire(&cpu->queue_lock);
    struct thread *first = cpu->runqueue;
    struct thread *curr = first;
    if (curr) {
      do {
        // TID
        char tid_buf[10];
        int j = 0;
        uint32_t tid = curr->tid;
        if (tid == 0) {
          tid_buf[j++] = '0';
        } else {
          while (tid > 0) {
            tid_buf[j++] = '0' + (tid % 10);
            tid /= 10;
          }
        }
        while (j < 4)
          tid_buf[j++] = ' ';
        for (int k = j - 1; k >= 0; k--)
          console_putchar(tid_buf[k]);
        console_putchar(' ');

        // CPU
        console_putchar('0' + (cpu->cpu_id % 10));
        console_puts("    ");

        // STATE
        switch (curr->state) {
        case THREAD_RUNNING:
          console_puts("RUNNING   ");
          break;
        case THREAD_READY:
          console_puts("READY     ");
          break;
        case THREAD_BLOCKED:
          console_puts("BLOCKED   ");
          break;
        case THREAD_SLEEPING:
          console_puts("SLEEPING  ");
          break;
        case THREAD_DEAD:
          console_puts("DEAD      ");
          break;
        case THREAD_ZOMBIE:
          console_puts("ZOMBIE    ");
          break;
        }

        // RSP (Hex)
        console_puts("0x");
        uint64_t rsp = curr->rsp;
        for (int bit = 60; bit >= 0; bit -= 4) {
          int nibble = (rsp >> bit) & 0xF;
          if (nibble < 10)
            console_putchar('0' + nibble);
          else
            console_putchar('A' + (nibble - 10));
        }
        console_putchar('\n');

        curr = curr->next;
      } while (curr != first);
    }
    spinlock_release(&cpu->queue_lock);
    __asm__ volatile("sti");
  }
}

bool sched_terminate_thread(uint32_t tid) {
  // Cannot kill idle threads
  // (We check by TID since we'll search for the thread by TID first)

  for (uint32_t i = 0; i < cpu_get_count(); i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
      continue;

    __asm__ volatile("cli");
    spinlock_acquire(&cpu->queue_lock);
    struct thread *first = cpu->runqueue;
    struct thread *curr = first;
    if (curr) {
      do {
        if (curr->tid == tid && !curr->is_idle) {
          curr->state = THREAD_DEAD;
          spinlock_release(&cpu->queue_lock);
          __asm__ volatile("sti");
          return true;
        }
        curr = curr->next;
      } while (curr != first);
    }
    spinlock_release(&cpu->queue_lock);
    __asm__ volatile("sti");
  }
  return false;
}

// Helper: remove thread from global thread list
static void remove_from_global_list(struct thread *t) {
  if (!global_thread_list)
    return;

  spinlock_acquire(&tid_lock);

  if (global_thread_list == t) {
    global_thread_list = t->global_next;
  } else {
    struct thread *prev = global_thread_list;
    while (prev && prev->global_next != t) {
      prev = prev->global_next;
    }
    if (prev) {
      prev->global_next = t->global_next;
    }
  }

  spinlock_release(&tid_lock);
}

// Helper: remove thread from its CPU's runqueue
static void remove_from_runqueue(struct thread *t) {
  // Find which CPU has this thread
  for (uint32_t i = 0; i < cpu_get_count(); i++) {
    struct cpu_info *cpu = cpu_get_info(i);
    if (!cpu || cpu->status == CPU_STATUS_OFFLINE)
      continue;

    __asm__ volatile("cli");
    spinlock_acquire(&cpu->queue_lock);
    struct thread *first = cpu->runqueue;

    if (!first) {
      spinlock_release(&cpu->queue_lock);
      __asm__ volatile("sti");
      continue;
    }

    // Check if this thread is in this CPU's runqueue
    struct thread *curr = first;
    bool found = false;
    do {
      if (curr == t) {
        found = true;
        break;
      }
      curr = curr->next;
    } while (curr != first);

    if (!found) {
      spinlock_release(&cpu->queue_lock);
      __asm__ volatile("sti");
      continue;
    }

    // Remove from circular list
    if (t->next == t) {
      // Only thread in queue
      cpu->runqueue = NULL;
    } else {
      // Find predecessor
      struct thread *prev = first;
      while (prev->next != t) {
        prev = prev->next;
      }
      prev->next = t->next;
      // Update runqueue head if needed
      if (cpu->runqueue == t) {
        cpu->runqueue = t->next;
      }
    }

    spinlock_release(&cpu->queue_lock);
    __asm__ volatile("sti");
    return;
  }
}

void sched_reparent_children(struct thread *parent) {
  if (!parent)
    return;

  spinlock_acquire(&tid_lock);
  struct thread *child = parent->children;
  while (child) {
    struct thread *next_sibling = child->sibling_next;

    // Reparent to BSP idle thread (TID 1) as a fallback for init
    // In a mature kernel, this would be the actual 'init' process.
    struct thread *init = global_thread_list;
    while (init && init->tid != 1) {
      init = init->global_next;
    }

    child->parent = init;
    if (init) {
      child->sibling_next = init->children;
      init->children = child;
    } else {
      child->sibling_next = NULL;
    }

    child = next_sibling;
  }
  parent->children = NULL;
  spinlock_release(&tid_lock);
}

void sched_reap_thread(struct thread *t) {
  if (!t || t->is_idle)
    return;

  klog_puts("[REAP] Reaping thread ");
  klog_uint64(t->tid);
  klog_puts("\n");

  // 1. Remove from lists (global, parent hierarchy, runqueue)
  // We do runqueue first as it uses CPU locks, then global/parent using
  // tid_lock.
  klog_puts("[REAP] Step 1: remove from runqueue\n");
  remove_from_runqueue(t);

  // 1.25 Ensure the thread is not currently active on any CPU (race prevention)
  for (uint32_t i = 0; i < cpu_get_count(); i++) {
    struct cpu_info *cpu_local = cpu_get_info(i);
    while (cpu_local->current_thread == t) {
      __asm__ volatile("pause");
    }
  }

  klog_puts("[REAP] Step 2: remove from lists\n");
  spinlock_acquire(&tid_lock);

  // 1.5 Remove from global thread list
  if (global_thread_list == t) {
    global_thread_list = t->global_next;
  } else {
    struct thread *prev_g = global_thread_list;
    while (prev_g && prev_g->global_next != t)
      prev_g = prev_g->global_next;
    if (prev_g)
      prev_g->global_next = t->global_next;
  }

  // 1.75 Remove from parent's children list
  if (t->parent) {
    if (t->parent->children == t) {
      t->parent->children = t->sibling_next;
    } else {
      struct thread *p = t->parent->children;
      while (p && p->sibling_next != t)
        p = p->sibling_next;
      if (p)
        p->sibling_next = t->sibling_next;
    }
  }
  spinlock_release(&tid_lock);

  // 3. Free fork_ctx (saved register state)
  klog_puts("[REAP] Step 3: free fork_ctx\n");

  // 4. Free user page tables (CR3) if still set
  klog_puts("[REAP] Step 4: free CR3=");
  klog_uint64(t->cr3);
  klog_puts("\n");
  if (t->cr3) {
    vmm_free_user_pages(t->cr3);
    t->cr3 = 0;
  }

  // Deferred Free: Free any previously deferred dead threads.
  // By the time a new thread is being reaped, any previously dead threads
  // have long been switched away from, guaranteeing their stacks are safe to
  // free.
  spinlock_acquire(&dead_threads_lock);
  struct dead_thread_info *curr_dead = dead_threads;
  dead_threads = NULL;
  spinlock_release(&dead_threads_lock);

  while (curr_dead) {
    if (curr_dead->stack_base) {
      kfree((void *)curr_dead->stack_base);
    }
    if (curr_dead->thread_ptr) {
      kfree((void *)curr_dead->thread_ptr);
    }
    struct dead_thread_info *to_free = curr_dead;
    curr_dead = curr_dead->next;
    kfree(to_free);
  }

  // Defer Freeing for THIS thread
  struct dead_thread_info *dead_info = kmalloc(sizeof(struct dead_thread_info));
  if (dead_info) {
    dead_info->stack_base = t->stack_base;
    dead_info->thread_ptr = (uint64_t)t;
    spinlock_acquire(&dead_threads_lock);
    dead_info->next = dead_threads;
    dead_threads = dead_info;
    spinlock_release(&dead_threads_lock);
    t->stack_base = 0;
  }

  klog_puts("[REAP] Done\n");
}
