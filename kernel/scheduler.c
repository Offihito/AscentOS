// scheduler.c - Task Scheduler Implementation (SIMPLIFIED)
#include "scheduler.h"
#include "task.h"
#include "timer.h"
#include "pmm.h"
#include "heap.h"
#include "signal64.h"   // signal_dispatch_pending (v10)
#include "spinlock64.h" // spinlock_t — switch_pending / pending_next_task koruması

// External functions
extern void serial_print(const char* str);
extern void int_to_str(int num, char* str);
extern uint64_t get_system_ticks(void);
extern void task_increment_ticks(void);

// Debug log — yalnızca SCHED_DEBUG tanımlıysa aktif
// Normal çalışmada serial_print devre dışı: 1000Hz × yazma = COM1 darboğazı
#ifdef SCHED_DEBUG
  #define SCHED_LOG(s) serial_print(s)
#else
  #define SCHED_LOG(s) ((void)0)
#endif

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
// spinlock ile korunur — SMP'de iki core aynı anda yazabilir
static spinlock_t sched_lock = SPINLOCK_INIT;
static int switch_pending = 0;
static task_t* pending_next_task = NULL;

// Current task tracking
task_t* previous_task = NULL;

// ===========================================
// SCHEDULER INITIALIZATION
// ===========================================

void scheduler_init(void) {
    if (scheduler_initialized) {
        SCHED_LOG("[SCHEDULER] Already initialized\n");
        return;
    }

    SCHED_LOG("[SCHEDULER] Initializing scheduler...\n");

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

    SCHED_LOG("[SCHEDULER] Scheduler initialized\n");
}

void scheduler_start(void) {
    if (!scheduler_initialized) {
        serial_print("[SCHEDULER ERROR] Not initialized!\n");  // hata: her zaman yaz
        return;
    }
    if (scheduler_running) {
        SCHED_LOG("[SCHEDULER] Already running\n");
        return;
    }
    SCHED_LOG("[SCHEDULER] Starting scheduler...\n");
    scheduler_running = 1;
    SCHED_LOG("[SCHEDULER] Scheduler is now active\n");
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
    uint64_t flags = spinlock_lock_irq(&sched_lock);

    if (!switch_pending || !pending_next_task) {
        spinlock_unlock_irq(&sched_lock, flags);
        return NULL;
    }

    task_t* current = task_get_current();
    task_t* next = pending_next_task;

    SCHED_LOG("[SCHEDULER] Switching: ");
    SCHED_LOG(current ? current->name : "NULL");
    SCHED_LOG(" -> ");
    SCHED_LOG(next->name);
    SCHED_LOG("\n");

    task_set_current(next);
    next->state = TASK_STATE_RUNNING;
    next->context_switches++;

    switch_pending = 0;
    pending_next_task = NULL;

    stats.total_context_switches++;
    tss_set_kernel_stack(next->kernel_stack_top);

    spinlock_unlock_irq(&sched_lock, flags);

    // Context switch tamamlandı — bekleyen sinyalleri işle
    // Burada çağrılır çünkü artık yeni task çalışıyor
    signal_dispatch_pending();

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
    // TSS.RSP0 artık task_get_next_context() içinde güncelleniyor
    // (next->kernel_stack_top kullanılarak — context.rsp + 160 YANLIŞ).
    // Bu fonksiyon isr_timer'dan çağrılmaya devam ediyor; no-op bırakıldı.
    (void)ctx;
}

// ===========================================
// SCHEDULER TICK
// ===========================================

void scheduler_tick(void) {
    if (!scheduler_initialized) return;

    task_increment_ticks();

    if (!scheduler_running)
        scheduler_running = 1;

    stats.total_ticks++;

    // Uyku süresi dolan task'ları READY'e al
    task_wakeup_check();

    task_t* current = task_get_current();

    // Orphan zombie temizle:
    // task_exit() task'ı ZOMBIE yapıp zombie_list'e ekler.
    // Eğer parent_pid == 0 veya parent artık yoksa (init/idle gibi)
    // waitpid() hiç gelmeyecektir; bir sonraki tick'te hemen reap et.
    // task_reap_zombie() doğru allocator'ı (pmm_free_pages + kfree) kullanır.
    if (previous_task && previous_task->state == TASK_STATE_ZOMBIE) {
        task_t* parent = task_find_by_pid(previous_task->parent_pid);
        int orphan = (parent == NULL || previous_task->parent_pid == 0);
        if (orphan) {
            SCHED_LOG("[SCHEDULER] Reaping orphan zombie task\n");
            task_reap_zombie(previous_task);
        }
        // parent varsa waitpid() toplayacak — dokunma
        previous_task = NULL;
    }

    if (!current) {
        task_t* next = scheduler_pick_next_task();
        if (next) {
            uint64_t flags = spinlock_lock_irq(&sched_lock);
            pending_next_task = next;
            switch_pending = 1;
            spinlock_unlock_irq(&sched_lock, flags);
        }
        return;
    }

    current->time_used++;

    if (!preemption_enabled)
        return;

    // BLOCKED/SLEEPING task: hemen switch
    if (current->state == TASK_STATE_BLOCKED ||
        current->state == TASK_STATE_SLEEPING) {
        task_t* next = scheduler_pick_next_task();
        if (next && next != current) {
            previous_task = current;
            uint64_t flags = spinlock_lock_irq(&sched_lock);
            pending_next_task = next;
            switch_pending = 1;
            spinlock_unlock_irq(&sched_lock, flags);
        }
        return;
    }

    // Zaman dilimi doldu
    if (current->time_used >= time_quantum) {
        task_t* next = scheduler_pick_next_task();

        if (next && next != current) {
            SCHED_LOG("[SCHEDULER] Time slice expired\n");

            if (current->pid != 0) {
                current->time_used = 0;
                current->state = TASK_STATE_READY;
                scheduler_add_task(current);
            } else {
                current->time_used = 0;
            }

            previous_task = current;
            uint64_t flags = spinlock_lock_irq(&sched_lock);
            pending_next_task = next;
            switch_pending = 1;
            spinlock_unlock_irq(&sched_lock, flags);
        } else {
            current->time_used = 0;
        }
    }

    if (current && current->pid == 0)
        stats.idle_ticks++;

    // NOT: signal_dispatch_pending() buradan kaldırıldı.
    // task_get_next_context() içinde, gerçek context switch
    // tamamlandıktan sonra çağrılıyor — daha doğru zamanlama.
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
        if (current->state != TASK_STATE_SLEEPING)
            current->state = TASK_STATE_READY;
        scheduler_add_task(current);
    }

    task_t* next = scheduler_pick_next_task();
    if (next && next != current) {
        uint64_t flags = spinlock_lock_irq(&sched_lock);
        pending_next_task = next;
        switch_pending = 1;
        spinlock_unlock_irq(&sched_lock, flags);
    }
}

void scheduler_block_current(void) {
    task_t* current = task_get_current();
    if (!current || current->pid == 0) return;

    current->state = TASK_STATE_BLOCKED;

    task_t* next = scheduler_pick_next_task();
    if (next) {
        uint64_t flags = spinlock_lock_irq(&sched_lock);
        pending_next_task = next;
        switch_pending = 1;
        spinlock_unlock_irq(&sched_lock, flags);
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
        SCHED_LOG("[SCHEDULER] Mode changed\n");
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
    SCHED_LOG("[SCHEDULER] Preemption enabled\n");
}

void scheduler_disable_preemption(void) {
    preemption_enabled = 0;
    SCHED_LOG("[SCHEDULER] Preemption disabled\n");
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