// scheduler.h - Task Scheduler for AscentOS 64-bit
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "task.h"

// Scheduler modes
#define SCHED_MODE_ROUND_ROBIN    0
#define SCHED_MODE_PRIORITY       1

// Time quantum (in ticks)
#define DEFAULT_TIME_QUANTUM      10  // 10ms @ 1000Hz

// Scheduler statistics
typedef struct {
    uint64_t total_context_switches;
    uint64_t total_ticks;
    uint64_t idle_ticks;
    uint32_t scheduler_mode;
    uint32_t time_quantum;
} scheduler_stats_t;

// ===========================================
// SCHEDULER INITIALIZATION
// ===========================================

// Initialize the scheduler
void scheduler_init(void);

// Start the scheduler (called after all initial tasks are created)
void scheduler_start(void);

// Check if scheduler is running
int scheduler_is_running(void);

// ===========================================
// SCHEDULER TICK (Called by Timer ISR)
// ===========================================

// Main scheduler tick function - called every timer interrupt
void scheduler_tick(void);

// ===========================================
// INTERRUPT-BASED CONTEXT SWITCHING
// ===========================================

// Check if context switch is pending (called from assembly interrupt handler)
int task_needs_switch(void);

// Save current task's stack pointer before switching (called from assembly)
void task_save_current_stack(uint64_t stack_pointer);

// Get next task's context for interrupt-based switch (called from assembly)
// Returns pointer to cpu_context_t of next task, or NULL if no switch needed
cpu_context_t* task_get_next_context(void);

// ===========================================
// TASK SCHEDULING
// ===========================================

// Schedule next task to run (round-robin)
task_t* scheduler_pick_next_task(void);

// Add task to scheduler (make it schedulable)
void scheduler_add_task(task_t* task);

// Remove task from scheduler
void scheduler_remove_task(task_t* task);

// Yield CPU to another task
void scheduler_yield(void);

// Block current task (remove from ready queue)
void scheduler_block_current(void);

// Unblock a task (add back to ready queue)
void scheduler_unblock_task(task_t* task);

// ===========================================
// SCHEDULER CONFIGURATION
// ===========================================

// Set scheduler mode
void scheduler_set_mode(uint32_t mode);

// Get scheduler mode
uint32_t scheduler_get_mode(void);

// Set time quantum
void scheduler_set_time_quantum(uint32_t ticks);

// Get time quantum
uint32_t scheduler_get_time_quantum(void);

// Enable/disable preemption
void scheduler_enable_preemption(void);
void scheduler_disable_preemption(void);
int scheduler_is_preemption_enabled(void);

// ===========================================
// STATISTICS & DEBUGGING
// ===========================================

// Get scheduler statistics
void scheduler_get_stats(scheduler_stats_t* stats);

// Get total context switches
uint64_t scheduler_get_context_switches(void);

// Print scheduler information
void scheduler_print_info(void);

// Reset statistics
void scheduler_reset_stats(void);

// ===========================================
// CONTEXT SWITCH (Assembly interface)
// ===========================================

// Perform actual context switch (implemented in interrupts64.asm)
extern void task_switch_context(cpu_context_t* old_ctx, cpu_context_t* new_ctx);

// Save current context (implemented in interrupts64.asm)
extern void task_save_current_context(cpu_context_t* ctx);

// Load and jump to context (implemented in interrupts64.asm)
extern void task_load_and_jump_context(cpu_context_t* ctx);

#endif // SCHEDULER_H