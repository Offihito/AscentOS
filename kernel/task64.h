// task64.h - AscentOS Multitasking System
#ifndef TASK64_H
#define TASK64_H

#include <stdint.h>
#include <stddef.h>

#define MAX_TASKS 32
#define TASK_STACK_SIZE 8192  // 8KB stack per task
#define TASK_NAME_LEN 32

// Task states
typedef enum {
    TASK_STATE_READY = 0,
    TASK_STATE_RUNNING,
    TASK_STATE_BLOCKED,
    TASK_STATE_SLEEPING,
    TASK_STATE_TERMINATED
} TaskState;

// CPU register context for task switching
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip;      // Instruction pointer
    uint64_t cs;       // Code segment
    uint64_t rflags;   // CPU flags
    uint64_t rsp;      // Stack pointer
    uint64_t ss;       // Stack segment
} __attribute__((packed)) CPUContext;

// Task Control Block
typedef struct Task {
    uint32_t pid;                    // Process ID
    char name[TASK_NAME_LEN];        // Task name
    TaskState state;                 // Current state
    uint8_t priority;                // Priority (0-255, higher = more priority)
    
    // CPU context
    CPUContext context;
    uint8_t* stack;                  // Stack base pointer
    
    // Statistics
    uint64_t cpu_time;               // Total CPU time used (in ticks)
    uint64_t start_time;             // Task start time
    uint64_t sleep_until;            // Wake up time (for sleeping tasks)
    
    // Scheduling
    struct Task* next;               // Next task in list
    
    // Parent/Child relationship
    uint32_t parent_pid;
    
    // Exit code
    int exit_code;
} Task;

// Task management functions
void task_init(void);
Task* task_create(const char* name, void (*entry_point)(void), uint8_t priority);
void task_terminate(Task* task, int exit_code);
void task_sleep(uint64_t milliseconds);
void task_yield(void);

// Task queries
Task* task_get_current(void);
Task* task_get_by_pid(uint32_t pid);
int task_get_all(Task** tasks, int max_tasks);
int task_count(void);

// Scheduler
void scheduler_init(void);
void scheduler_tick(void);
Task* scheduler_select_next(void);
void task_switch(Task* old_task, Task* new_task);

// Statistics
uint32_t task_get_cpu_usage(Task* task);
uint64_t task_get_uptime(Task* task);

// Demo tasks
void demo_task_counter(void);
void demo_task_spinner(void);
void demo_task_calculator(void);

#endif