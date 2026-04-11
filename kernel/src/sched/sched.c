#include "sched.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../lib/string.h"
#include "../console/console.h"
#include "../console/klog.h"
#include "../cpu/idt.h"
#include "../smp/cpu.h"
#include "../lock/spinlock.h"

static uint32_t next_tid = 1;
static spinlock_t tid_lock = SPINLOCK_INIT;

// Global list of all threads (for wait4)
struct thread *global_thread_list = NULL;

extern void switch_context(uint64_t *old_sp, uint64_t new_sp);
extern void thread_stub(void); // Defined in switch.asm

void sched_init(void) {
    // We expect this to be called after cpu_init() which populates the CPU list
    uint32_t count = cpu_get_count();
    
    for (uint32_t i = 0; i < count; i++) {
        struct cpu_info *cpu = cpu_get_info(i);
        if (!cpu || cpu->status == CPU_STATUS_OFFLINE) continue;
        
        // Create the idle thread for this specific CPU
        struct thread *idle_thread = kmalloc(sizeof(struct thread));
        memset(idle_thread, 0, sizeof(struct thread));
        // Assign a proper TID to the idle thread (don't use 0)
        spinlock_acquire(&tid_lock);
        idle_thread->tid = next_tid++;
        spinlock_release(&tid_lock);
        idle_thread->is_idle = true;
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
    if(cpu->current_thread) {
        cpu->current_thread->state = THREAD_DEAD;
    }
    // Infinite loop, yield will switch away
    while(1) {
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

    spinlock_acquire(&target_cpu->queue_lock);
    
    if (!target_cpu->runqueue) {
        target_cpu->runqueue = t;
        t->next = t;
    } else {
        struct thread *tail = target_cpu->runqueue;
        while(tail->next != target_cpu->runqueue) {
            tail = tail->next;
        }
        tail->next = t;
        t->next = target_cpu->runqueue;
    }
    
    spinlock_release(&target_cpu->queue_lock);
}

struct thread *sched_create_kernel_thread(void (*entry)(void), struct cpu_info *explicit_cpu) {
    // Allocate thread struct
    struct thread *t = kmalloc(sizeof(struct thread));
    if(!t) return NULL;
    
    memset(t, 0, sizeof(struct thread));
    vma_list_init(&t->vmas);
    
    spinlock_acquire(&tid_lock);
    t->tid = next_tid++;
    t->global_next = global_thread_list;
    global_thread_list = t;
    spinlock_release(&tid_lock);
    
    // Set parent to the thread that called create
    t->parent = sched_get_current();
    
    t->state = THREAD_READY;
    t->stack_size = THREAD_STACK_SIZE;
    
    t->stack_base = (uint64_t)kmalloc(THREAD_STACK_SIZE);
    
    if(!t->stack_base) {
        kfree(t);
        return NULL;
    }
    
    uint64_t stack_top = t->stack_base + THREAD_STACK_SIZE;
    stack_top &= ~0xFULL; // Align stack
    
    // 1. Push the thread exit function (simulating a return address)
    stack_top -= 8;
    *(uint64_t*)stack_top = (uint64_t)thread_exit;
    
    // 2. Setup context frame
    stack_top -= sizeof(struct context);
    struct context *ctx = (struct context *)stack_top;
    memset(ctx, 0, sizeof(struct context));
    
    // Store entry function in r12 which thread_stub will call
    ctx->r12 = (uint64_t)entry;
    
    // `switch_context` does `ret`, popping this address
    ctx->ret_addr = (uint64_t)thread_stub;
    
    t->rsp = stack_top;
    
    // Balance and add to a CPU's runqueue
    sched_enqueue_thread(t, explicit_cpu);
    
    return t;
}

void sched_yield(void) {
    __asm__ volatile("cli");
    struct cpu_info *cpu = cpu_get_current();
    if(!cpu->current_thread) {
         __asm__ volatile("sti");
         return;
    }
    
    struct thread *prev = cpu->current_thread;
    
    // Use local pointers to prevent breaking if queue is modified
    spinlock_acquire(&cpu->queue_lock);
    struct thread *next_t = cpu->current_thread->next;
    
    // Find next ready/running thread
    while(next_t != cpu->current_thread) {
        if(next_t->state == THREAD_READY || next_t->state == THREAD_RUNNING) {
            break;
        }
        next_t = next_t->next;
    }
    
    if (next_t != cpu->current_thread && (next_t->state == THREAD_READY || next_t->state == THREAD_RUNNING)) {
        if (prev->state == THREAD_RUNNING) {
            prev->state = THREAD_READY;
        }
        next_t->state = THREAD_RUNNING;
        cpu->current_thread = next_t;
        
        cpu->stack_top = next_t->stack_base + next_t->stack_size;
        extern void tss_set_rsp0(uint64_t rsp0);
        tss_set_rsp0(cpu->stack_top);
        
        uint64_t target_cr3 = next_t->cr3 ? next_t->cr3 : cpu->kernel_cr3;
        uint64_t current_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
        if (target_cr3 != current_cr3) {
            __asm__ volatile("mov %0, %%cr3" :: "r"(target_cr3) : "memory");
        }
        
        spinlock_release(&cpu->queue_lock);
        switch_context(&prev->rsp, next_t->rsp);
    } else {
        spinlock_release(&cpu->queue_lock);
    }
    
    // Only enable here if we are returning normally
    __asm__ volatile("sti");
}

void sched_tick(struct registers *regs) {
    (void)regs;
    struct cpu_info *cpu = cpu_get_current();
    if(cpu->current_thread) {
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
        if (!cpu || cpu->status == CPU_STATUS_OFFLINE) continue;
        
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
                while (j < 4) tid_buf[j++] = ' ';
                for (int k = j - 1; k >= 0; k--) console_putchar(tid_buf[k]);
                console_putchar(' ');

                // CPU
                console_putchar('0' + (cpu->cpu_id % 10));
                console_puts("    ");

                // STATE
                switch (curr->state) {
                    case THREAD_RUNNING: console_puts("RUNNING   "); break;
                    case THREAD_READY:   console_puts("READY     "); break;
                    case THREAD_BLOCKED: console_puts("BLOCKED   "); break;
                    case THREAD_SLEEPING:console_puts("SLEEPING  "); break;
                    case THREAD_DEAD:    console_puts("DEAD      "); break;
                    case THREAD_ZOMBIE:  console_puts("ZOMBIE    "); break;
                }

                // RSP (Hex)
                console_puts("0x");
                uint64_t rsp = curr->rsp;
                for (int bit = 60; bit >= 0; bit -= 4) {
                    int nibble = (rsp >> bit) & 0xF;
                    if (nibble < 10) console_putchar('0' + nibble);
                    else console_putchar('A' + (nibble - 10));
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
        if (!cpu || cpu->status == CPU_STATUS_OFFLINE) continue;

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
