// scheduler.c - Task Scheduler Implementation
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

// Current task tracking
static task_t* previous_task = NULL;

// ===========================================
// SCHEDULER INITIALIZATION
// ===========================================

void scheduler_init(void) {
    if (scheduler_initialized) {
        serial_print("[SCHEDULER] Already initialized\n");
        return;
    }
    
    serial_print("[SCHEDULER] Initializing scheduler...\n");
    
    // Reset statistics
    stats.total_context_switches = 0;
    stats.total_ticks = 0;
    stats.idle_ticks = 0;
    stats.scheduler_mode = SCHED_MODE_ROUND_ROBIN;
    stats.time_quantum = DEFAULT_TIME_QUANTUM;
    
    scheduler_mode = SCHED_MODE_ROUND_ROBIN;
    time_quantum = DEFAULT_TIME_QUANTUM;
    preemption_enabled = 1;
    
    scheduler_initialized = 1;
    scheduler_running = 0;  // Not started yet
    
    serial_print("[SCHEDULER] Scheduler initialized (Round-Robin, 10ms quantum)\n");
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
// SCHEDULER TICK - Heart of the Scheduler
// ===========================================

void scheduler_tick(void) {
    if (!scheduler_initialized) return;
    
    // Increment system ticks
    task_increment_ticks();
    
    // Auto-start scheduler on first tick
    if (!scheduler_running) {
        scheduler_running = 1;
    }
    
    stats.total_ticks++;
    
    // Get current task
    task_t* current = task_get_current();
    if (!current) {
        // No current task, pick one
        task_t* next = scheduler_pick_next_task();
        if (next) {
            task_switch(NULL, next);
            stats.total_context_switches++;
        }
        return;
    }
    
    // Update current task's time
    current->time_used++;
    
    // Check if preemption is disabled
    if (!preemption_enabled) {
        return;
    }
    
    // Check if time slice expired
    if (current->time_used >= time_quantum) {
        // Time slice expired, switch to next task
        task_t* next = scheduler_pick_next_task();
        
        if (next && next != current) {
            // Put current task back in ready queue (if not idle)
            if (current->pid != 0) {  // Not idle task
                current->time_used = 0;
                current->state = TASK_STATE_READY;
                scheduler_add_task(current);
            }
            
            // Switch to next task
            previous_task = current;
            task_switch(current, next);
            stats.total_context_switches++;
        } else {
            // No other task or same task, reset time
            current->time_used = 0;
        }
    }
    
    // Track idle time
    if (current->pid == 0) {
        stats.idle_ticks++;
    }
}

// ===========================================
// TASK SCHEDULING ALGORITHMS
// ===========================================

task_t* scheduler_pick_next_task(void) {
    task_t* next = NULL;
    
    switch (scheduler_mode) {
        case SCHED_MODE_ROUND_ROBIN:
            // Get next task from ready queue
            next = task_get_next();
            break;
            
        case SCHED_MODE_PRIORITY:
            // TODO: Implement priority-based scheduling
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
    
    // Add to ready queue
    task_start(task);
}

void scheduler_remove_task(task_t* task) {
    if (!task) return;
    
    // Remove from ready queue
    // This is handled by task_queue_remove in task.c
}

void scheduler_yield(void) {
    // Voluntarily give up CPU
    task_t* current = task_get_current();
    if (!current) return;
    
    // Put current task back in ready queue
    if (current->pid != 0) {  // Not idle
        current->time_used = 0;
        current->state = TASK_STATE_READY;
        scheduler_add_task(current);
    }
    
    // Pick next task
    task_t* next = scheduler_pick_next_task();
    if (next && next != current) {
        task_switch(current, next);
        stats.total_context_switches++;
    }
}

void scheduler_block_current(void) {
    task_t* current = task_get_current();
    if (!current || current->pid == 0) return;  // Can't block idle
    
    current->state = TASK_STATE_BLOCKED;
    
    // Pick next task and switch
    task_t* next = scheduler_pick_next_task();
    if (next) {
        task_switch(current, next);
        stats.total_context_switches++;
    }
}

void scheduler_unblock_task(task_t* task) {
    if (!task) return;
    
    task->state = TASK_STATE_READY;
    scheduler_add_task(task);
}

// ===========================================
// SCHEDULER CONFIGURATION
// ===========================================

void scheduler_set_mode(uint32_t mode) {
    if (mode <= SCHED_MODE_PRIORITY) {
        scheduler_mode = mode;
        stats.scheduler_mode = mode;
        
        serial_print("[SCHEDULER] Mode changed to: ");
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
    if (ticks > 0 && ticks < 1000) {  // Reasonable limits
        time_quantum = ticks;
        stats.time_quantum = ticks;
        
        char msg[64];
        msg[0] = '[';
        msg[1] = 'S';
        msg[2] = 'C';
        msg[3] = 'H';
        msg[4] = 'E';
        msg[5] = 'D';
        msg[6] = ']';
        msg[7] = ' ';
        msg[8] = 'T';
        msg[9] = 'i';
        msg[10] = 'm';
        msg[11] = 'e';
        msg[12] = ' ';
        msg[13] = 'q';
        msg[14] = 'u';
        msg[15] = 'a';
        msg[16] = 'n';
        msg[17] = 't';
        msg[18] = 'u';
        msg[19] = 'm';
        msg[20] = ':';
        msg[21] = ' ';
        
        char num[16];
        int_to_str(ticks, num);
        int i = 0;
        while (num[i]) {
            msg[22 + i] = num[i];
            i++;
        }
        msg[22 + i] = 'm';
        msg[23 + i] = 's';
        msg[24 + i] = '\n';
        msg[25 + i] = '\0';
        serial_print(msg);
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
// STATISTICS & DEBUGGING
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
    serial_print("\n=== Scheduler Information ===\n");
    
    char msg[128];
    char num[32];
    
    // Mode
    serial_print("Mode: ");
    if (scheduler_mode == SCHED_MODE_ROUND_ROBIN) {
        serial_print("Round-Robin\n");
    } else {
        serial_print("Priority\n");
    }
    
    // Time quantum
    serial_print("Time quantum: ");
    int_to_str(time_quantum, num);
    serial_print(num);
    serial_print(" ticks\n");
    
    // Preemption
    serial_print("Preemption: ");
    if (preemption_enabled) {
        serial_print("Enabled\n");
    } else {
        serial_print("Disabled\n");
    }
    
    // Statistics
    serial_print("\n=== Statistics ===\n");
    
    serial_print("Total ticks: ");
    // Simple uint64 to string (limited to reasonable values)
    uint64_t val = stats.total_ticks;
    if (val > 4294967295ULL) val = 4294967295ULL;  // Cap at uint32 max
    int_to_str((int)val, num);
    serial_print(num);
    serial_print("\n");
    
    serial_print("Context switches: ");
    val = stats.total_context_switches;
    if (val > 4294967295ULL) val = 4294967295ULL;
    int_to_str((int)val, num);
    serial_print(num);
    serial_print("\n");
    
    serial_print("Idle ticks: ");
    val = stats.idle_ticks;
    if (val > 4294967295ULL) val = 4294967295ULL;
    int_to_str((int)val, num);
    serial_print(num);
    serial_print("\n");
    
    // CPU usage (approximate)
    if (stats.total_ticks > 0) {
        uint32_t cpu_usage = 100;
        if (stats.idle_ticks > 0) {
            cpu_usage = 100 - ((stats.idle_ticks * 100) / stats.total_ticks);
        }
        serial_print("CPU usage: ~");
        int_to_str(cpu_usage, num);
        serial_print(num);
        serial_print("%\n");
    }
    
    serial_print("\n");
}

void scheduler_reset_stats(void) {
    stats.total_context_switches = 0;
    stats.total_ticks = 0;
    stats.idle_ticks = 0;
    
    serial_print("[SCHEDULER] Statistics reset\n");
}

// ===========================================
// ASSEMBLY STUBS (Will be implemented in interrupts64.asm)
// ===========================================

// These are weak symbols - if not provided by assembly, use these stubs
__attribute__((weak)) void task_switch_context(cpu_context_t* old_ctx, cpu_context_t* new_ctx) {
    (void)old_ctx;
    (void)new_ctx;
    // This will be implemented in interrupts64.asm
    serial_print("[SCHEDULER WARNING] Context switch not implemented!\n");
}

__attribute__((weak)) void task_save_current_context(cpu_context_t* ctx) {
    (void)ctx;
    // This will be implemented in interrupts64.asm
}

__attribute__((weak)) void task_load_and_jump_context(cpu_context_t* ctx) {
    (void)ctx;
    // This will be implemented in interrupts64.asm
}