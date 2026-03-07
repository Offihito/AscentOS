// scheduler.c - Task Scheduler Implementation (SIMPLIFIED)
#include "scheduler.h"
#include "task.h"
#include "timer.h"
#include "pmm.h"
#include "heap.h"
#include "signal64.h"   // signal_dispatch_pending (v10)

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

// ============================================================
// TSS RSP0 GÜNCELLEME — isr_timer context switch sonrası
// ============================================================
//
// isr_timer TSS.RSP0'ı task_switch() gibi güncellemez;
// task_get_next_context → cpu_context_t* döner ve isr_timer
// doğrudan RSP'yi değiştirir. Ring-3'ten interrupt/syscall
// geldiğinde CPU TSS.RSP0'ı kullanır — bu fonksiyon olmadan
// yanlış kernel stack'e geçilir → #DF → triple fault.
//
// cpu_context_t.rsp = kernel stack'teki frame pointer'ı.
// kernel_stack_top = context.rsp + (15 reg + 5 iretq) * 8 = context.rsp + 160
//
// Parametre: isr_timer'ın task_get_next_context'ten aldığı cpu_context_t*
// ============================================================
void tss_update_rsp0_from_context(cpu_context_t* ctx) {
    if (!ctx) return;
    // kernel_stack_top = context.rsp + 160 (15 register + 5 iretq qword)
    uint64_t kernel_stack_top = ctx->rsp + 160;
    tss_set_kernel_stack(kernel_stack_top);
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

        // Kernel stack: pmm_alloc_pages ile tahsis edildiyse pmm_free_pages ile serbest bırak.
        // kfree() ile serbest bırakmak heap'i bozar çünkü adres heap aralığında değil.
        if (previous_task->kernel_stack_base) {
            extern void pmm_free_pages(void* base, uint64_t count);
            extern uint8_t* heap_start;
            extern uint8_t* heap_current;
            uint8_t* ks = (uint8_t*)previous_task->kernel_stack_base;
            // Heap aralığında değilse pmm ile serbest bırak
            if (ks < heap_start || ks >= heap_current) {
                uint64_t page_count = previous_task->kernel_stack_size / 4096;
                pmm_free_pages((void*)previous_task->kernel_stack_base, page_count);
            } else {
                extern void kfree(void* ptr);
                kfree((void*)previous_task->kernel_stack_base);
            }
        }

        // User stack: pmm_alloc_pages_flags ile tahsis edildi
        if (previous_task->user_stack_base) {
            extern void pmm_free_pages(void* base, uint64_t count);
            extern uint8_t* heap_start;
            extern uint8_t* heap_current;
            uint8_t* us = (uint8_t*)previous_task->user_stack_base;
            if (us < heap_start || us >= heap_current) {
                uint64_t page_count = previous_task->user_stack_size / 4096;
                pmm_free_pages((void*)previous_task->user_stack_base, page_count);
            } else {
                extern void kfree(void* ptr);
                kfree((void*)previous_task->user_stack_base);
            }
        }

        // TCB kendisi heap'ten kmalloc ile alındı
        extern void kfree(void* ptr);
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

    // BLOCKED task: hemen switch — meşgul beklemeden kaçın
    if (current->state == TASK_STATE_BLOCKED ||
        current->state == TASK_STATE_SLEEPING) {
        task_t* next = scheduler_pick_next_task();
        if (next && next != current) {
            previous_task = current;
            pending_next_task = next;
            switch_pending = 1;
        }
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

    // Context switch tamamlandıktan sonra yeni task'ın bekleyen
    // sinyallerini kontrol et ve işle (v10 sinyal altyapısı)
    signal_dispatch_pending();
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
        // SLEEPING state'i koru — task_sleep() sonrası yield yapılıyorsa
        // state'i READY'e çevirme; scheduler_tick() wake_tick dolunca uyandırır.
        if (current->state != TASK_STATE_SLEEPING) {
            current->state = TASK_STATE_READY;
        }
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
