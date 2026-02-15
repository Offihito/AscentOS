// scheduler.c - Task Scheduler Implementation (SIMPLIFIED)
#include "scheduler.h"
#include "task.h"
#include "timer.h"
#include "memory_unified.h"

// External functions
extern void serial_print(const char* str);
extern void int_to_str(int num, char* str);
extern uint64_t get_system_ticks(void);
extern void task_increment_ticks(void);

// ===========================================
// GLOBAL VARIABLES
// ===========================================

static int scheduler_initialized = 0;
static int scheduler_running = 0;
static int preemption_enabled = 1;

static uint32_t scheduler_mode = SCHED_MODE_ROUND_ROBIN;
static uint32_t time_quantum = DEFAULT_TIME_QUANTUM;

static scheduler_stats_t stats;

// Context switch pending flag
static int switch_pending = 0;
static task_t* pending_next_task = NULL;

// Current task tracking
task_t* previous_task = NULL;

// ===========================================
// SCHEDULER INITIALIZATION
// ===========================================

void scheduler_init(void) {
    if (scheduler_initialized) {
        serial_print("[SCHEDULER] Already initialized\n");
        return;
    }
    
    serial_print("[SCHEDULER] Initializing scheduler...\n");
    
    stats.total_context_switches = 0;
    stats.total_ticks = 0;
    stats.idle_ticks = 0;
    stats.scheduler_mode = SCHED_MODE_ROUND_ROBIN;
    stats.time_quantum = DEFAULT_TIME_QUANTUM;
    
    scheduler_mode = SCHED_MODE_ROUND_ROBIN;
    time_quantum = DEFAULT_TIME_QUANTUM;
    preemption_enabled = 1;
    switch_pending = 0;
    pending_next_task = NULL;
    
    scheduler_initialized = 1;
    scheduler_running = 0;
    
    serial_print("[SCHEDULER] Scheduler initialized\n");
}

void scheduler_start(void) {
    if (!scheduler_initialized) {
        serial_print("[SCHEDULER ERROR] Not initialized!\n");
        return;
    }
    
    if (scheduler_running) {
        serial_print("[SCHEDULER] Already running\n");
        return;
    }
    
    serial_print("[SCHEDULER] Starting scheduler...\n");
    scheduler_running = 1;
    serial_print("[SCHEDULER] Scheduler is now active\n");
}

int scheduler_is_running(void) {
    return scheduler_running;
}

// ===========================================
// INTERRUPT-BASED CONTEXT SWITCHING
// ===========================================

int task_needs_switch(void) {
    return switch_pending;
}

// Save current task's stack pointer (called from interrupt)
void task_save_current_stack(uint64_t stack_pointer) {
    task_t* current = task_get_current();
    if (current && switch_pending) {
        current->context.rsp = stack_pointer;
    }
}

cpu_context_t* task_get_next_context(void) {
    if (!switch_pending || !pending_next_task) {
        return NULL;
    }
    
    task_t* current = task_get_current();
    task_t* next = pending_next_task;
    
    serial_print("[SCHEDULER] Switching: ");
    serial_print(current ? current->name : "NULL");
    serial_print(" -> ");
    serial_print(next->name);
    serial_print("\n");
    
    // Update current task
    task_set_current(next);
    next->state = TASK_STATE_RUNNING;
    next->context_switches++;
    
    // Clear pending switch
    switch_pending = 0;
    pending_next_task = NULL;
    
    stats.total_context_switches++;
    
    // Return pointer to new task's context
    return &next->context;
}

// ===========================================
// SCHEDULER TICK
// ===========================================

void scheduler_tick(void) {
    if (!scheduler_initialized) return;
    
    task_increment_ticks();
    
    if (!scheduler_running) {
        scheduler_running = 1;
    }
    
    stats.total_ticks++;
    
    task_t* current = task_get_current();
    
    // Clean up terminated tasks
    if (previous_task && previous_task->state == TASK_STATE_TERMINATED) {
        serial_print("[SCHEDULER] Cleaning up: ");
        serial_print(previous_task->name);
        serial_print("\n");
        
        if (previous_task->kernel_stack_base) {
            extern void kfree(void* ptr);
            kfree((void*)previous_task->kernel_stack_base);
        }
        kfree(previous_task);
        previous_task = NULL;
    }
    
    if (!current) {
        task_t* next = scheduler_pick_next_task();
        if (next) {
            pending_next_task = next;
            switch_pending = 1;
        }
        return;
    }
    
    current->time_used++;
    
    if (!preemption_enabled) {
        return;
    }
    
    if (current->time_used >= time_quantum) {
        task_t* next = scheduler_pick_next_task();
        
        if (next && next != current) {
            serial_print("[SCHEDULER] Time slice expired: ");
            serial_print(current->name);
            serial_print("\n");
            
            if (current->pid != 0) {
                current->time_used = 0;
                current->state = TASK_STATE_READY;
                scheduler_add_task(current);
            } else {
                current->time_used = 0;
            }
            
            previous_task = current;
            pending_next_task = next;
            switch_pending = 1;
        } else {
            current->time_used = 0;
        }
    }
    
    if (current && current->pid == 0) {
        stats.idle_ticks++;
    }
}

// ===========================================
// TASK SCHEDULING
// ===========================================

task_t* scheduler_pick_next_task(void) {
    task_t* next = NULL;
    
    switch (scheduler_mode) {
        case SCHED_MODE_ROUND_ROBIN:
            next = task_get_next();
            break;
        case SCHED_MODE_PRIORITY:
            next = task_get_next();
            break;
        default:
            next = task_get_next();
            break;
    }
    
    return next;
}

void scheduler_add_task(task_t* task) {
    if (!task) return;
    task_start(task);
}

void scheduler_remove_task(task_t* task) {
    if (!task) return;
}

void scheduler_yield(void) {
    task_t* current = task_get_current();
    if (!current) return;
    
    if (current->pid != 0) {
        current->time_used = 0;
        current->state = TASK_STATE_READY;
        scheduler_add_task(current);
    }
    
    task_t* next = scheduler_pick_next_task();
    if (next && next != current) {
        pending_next_task = next;
        switch_pending = 1;
    }
}

void scheduler_block_current(void) {
    task_t* current = task_get_current();
    if (!current || current->pid == 0) return;
    
    current->state = TASK_STATE_BLOCKED;
    
    task_t* next = scheduler_pick_next_task();
    if (next) {
        pending_next_task = next;
        switch_pending = 1;
    }
}

void scheduler_unblock_task(task_t* task) {
    if (!task) return;
    
    task->state = TASK_STATE_READY;
    scheduler_add_task(task);
}

// ===========================================
// CONFIGURATION
// ===========================================

void scheduler_set_mode(uint32_t mode) {
    if (mode <= SCHED_MODE_PRIORITY) {
        scheduler_mode = mode;
        stats.scheduler_mode = mode;
        
        serial_print("[SCHEDULER] Mode: ");
        if (mode == SCHED_MODE_ROUND_ROBIN) {
            serial_print("Round-Robin\n");
        } else {
            serial_print("Priority\n");
        }
    }
}

uint32_t scheduler_get_mode(void) {
    return scheduler_mode;
}

void scheduler_set_time_quantum(uint32_t ticks) {
    if (ticks > 0 && ticks < 1000) {
        time_quantum = ticks;
        stats.time_quantum = ticks;
    }
}

uint32_t scheduler_get_time_quantum(void) {
    return time_quantum;
}

void scheduler_enable_preemption(void) {
    preemption_enabled = 1;
    serial_print("[SCHEDULER] Preemption enabled\n");
}

void scheduler_disable_preemption(void) {
    preemption_enabled = 0;
    serial_print("[SCHEDULER] Preemption disabled\n");
}

int scheduler_is_preemption_enabled(void) {
    return preemption_enabled;
}

// ===========================================
// STATISTICS
// ===========================================

void scheduler_get_stats(scheduler_stats_t* out_stats) {
    if (!out_stats) return;
    
    out_stats->total_context_switches = stats.total_context_switches;
    out_stats->total_ticks = stats.total_ticks;
    out_stats->idle_ticks = stats.idle_ticks;
    out_stats->scheduler_mode = stats.scheduler_mode;
    out_stats->time_quantum = stats.time_quantum;
}

uint64_t scheduler_get_context_switches(void) {
    return stats.total_context_switches;
}

void scheduler_print_info(void) {
    serial_print("\n=== Scheduler Info ===\n");
    
    char num[32];
    
    serial_print("Mode: ");
    serial_print(scheduler_mode == SCHED_MODE_ROUND_ROBIN ? "Round-Robin\n" : "Priority\n");
    
    serial_print("Quantum: ");
    int_to_str(time_quantum, num);
    serial_print(num);
    serial_print(" ticks\n");
    
    serial_print("Preemption: ");
    serial_print(preemption_enabled ? "On\n" : "Off\n");
    
    serial_print("\nStats:\n");
    
    serial_print("Ticks: ");
    int_to_str((int)(stats.total_ticks & 0xFFFFFFFF), num);
    serial_print(num);
    serial_print("\n");
    
    serial_print("Switches: ");
    int_to_str((int)(stats.total_context_switches & 0xFFFFFFFF), num);
    serial_print(num);
    serial_print("\n");
    
    serial_print("Idle: ");
    int_to_str((int)(stats.idle_ticks & 0xFFFFFFFF), num);
    serial_print(num);
    serial_print("\n\n");
}

void scheduler_reset_stats(void) {
    stats.total_context_switches = 0;
    stats.total_ticks = 0;
    stats.idle_ticks = 0;
    serial_print("[SCHEDULER] Stats reset\n");
}

// ===========================================
// WEAK STUBS
// ===========================================

__attribute__((weak)) void task_switch_context(cpu_context_t* old_ctx, cpu_context_t* new_ctx) {
    (void)old_ctx; (void)new_ctx;
    serial_print("[SCHEDULER] No context switch!\n");
}

__attribute__((weak)) void task_save_current_context(cpu_context_t* ctx) {
    (void)ctx;
}

__attribute__((weak)) void task_load_and_jump_context(cpu_context_t* ctx) {
    (void)ctx;
}