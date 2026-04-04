#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include <stddef.h>
#include "../cpu/isr.h"

typedef enum {
    THREAD_RUNNING = 0,
    THREAD_READY,
    THREAD_BLOCKED,
    THREAD_DEAD
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
    struct thread *next;
};

void sched_init(void);

struct thread *sched_create_kernel_thread(void (*entry_point)(void));

void sched_tick(struct registers *regs);
void sched_yield(void);

// Returns the current thread *for the CPU currently executing this code*
struct thread *sched_get_current(void);

// Load balancing / dispatching
void sched_enqueue_thread(struct thread *t);

// Task management for shell
void sched_print_tasks(void);
bool sched_terminate_thread(uint32_t tid);

#endif
