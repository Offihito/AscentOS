// task.c - Task Management Implementation
#include "task.h"
#include "memory_unified.h"
#include <stddef.h>

// String utilities (commands64.c'den)
extern void str_cpy(char* dest, const char* src);
extern int str_len(const char* str);
extern void println64(const char* str, uint8_t color);
extern void print_str64(const char* str, uint8_t color);
extern void int_to_str(int num, char* str);
extern void uint64_to_string(uint64_t num, char* str);

// Serial output (debugging)
extern void serial_print(const char* str);

// Timer
extern uint64_t get_system_ticks(void);

// VGA colors
#define VGA_WHITE 0x0F
#define VGA_GREEN 0x0A
#define VGA_CYAN 0x03
#define VGA_YELLOW 0x0E

// ===========================================
// GLOBAL VARIABLES
// ===========================================

static task_t* current_task = NULL;      // Şu anda çalışan task
static task_queue_t ready_queue;         // Çalışmaya hazır task'lar
static task_t* idle_task = NULL;         // Idle task
static uint32_t next_pid = 1;            // Sonraki PID
static int task_system_initialized = 0;  // Sistem başlatıldı mı?

// ===========================================
// HELPER FUNCTIONS
// ===========================================

// Basit memset
static void* memset_task(void* dest, int val, uint64_t n) {
    uint8_t* d = (uint8_t*)dest;
    while (n--) *d++ = (uint8_t)val;
    return dest;
}

// String kopyalama (güvenli)
static void str_copy_safe(char* dest, const char* src, int max_len) {
    int i = 0;
    while (src[i] && i < max_len - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

// ===========================================
// TASK QUEUE OPERATIONS
// ===========================================

void task_queue_init(task_queue_t* queue) {
    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0;
}

void task_queue_push(task_queue_t* queue, task_t* task) {
    if (!task) return;
    
    task->next = NULL;
    task->prev = queue->tail;
    
    if (queue->tail) {
        queue->tail->next = task;
    } else {
        queue->head = task;
    }
    
    queue->tail = task;
    queue->count++;
}

task_t* task_queue_pop(task_queue_t* queue) {
    if (!queue->head) return NULL;
    
    task_t* task = queue->head;
    queue->head = task->next;
    
    if (queue->head) {
        queue->head->prev = NULL;
    } else {
        queue->tail = NULL;
    }
    
    task->next = NULL;
    task->prev = NULL;
    queue->count--;
    
    return task;
}

void task_queue_remove(task_queue_t* queue, task_t* task) {
    if (!task) return;
    
    if (task->prev) {
        task->prev->next = task->next;
    } else {
        queue->head = task->next;
    }
    
    if (task->next) {
        task->next->prev = task->prev;
    } else {
        queue->tail = task->prev;
    }
    
    task->next = NULL;
    task->prev = NULL;
    queue->count--;
}

int task_queue_is_empty(task_queue_t* queue) {
    return queue->count == 0;
}

// ===========================================
// TASK CREATION & INITIALIZATION
// ===========================================

void task_init(void) {
    if (task_system_initialized) return;
    
    serial_print("[TASK] Initializing task management system...\n");
    
    // Ready queue'yu başlat
    task_queue_init(&ready_queue);
    
    // IMPORTANT: Set initialized flag BEFORE creating idle task
    // because task_create_idle calls task_create which checks this flag
    task_system_initialized = 1;
    
    // Idle task oluştur
    idle_task = task_create_idle();
    if (idle_task) {
        serial_print("[TASK] Idle task created (PID=0)\n");
    } else {
        serial_print("[TASK ERROR] Failed to create idle task!\n");
        task_system_initialized = 0;  // Reset on failure
        return;
    }
    
    current_task = idle_task;
    
    serial_print("[TASK] Task system initialized\n");
}

task_t* task_create(const char* name, void (*entry_point)(void), uint32_t priority) {
    if (!task_system_initialized) {
        serial_print("[TASK ERROR] Task system not initialized!\n");
        return NULL;
    }
    
    // Task yapısı için bellek ayır
    task_t* task = (task_t*)kmalloc(sizeof(task_t));
    if (!task) {
        serial_print("[TASK ERROR] Failed to allocate task structure\n");
        return NULL;
    }
    
    // Yapıyı sıfırla
    memset_task(task, 0, sizeof(task_t));
    
    // Task bilgilerini ayarla
    task->pid = next_pid++;
    str_copy_safe(task->name, name, 32);
    task->state = TASK_STATE_READY;
    task->priority = priority;
    
    // Kernel stack ayır
    task->kernel_stack_size = KERNEL_STACK_SIZE;
    task->kernel_stack_base = (uint64_t)kmalloc(KERNEL_STACK_SIZE);
    if (!task->kernel_stack_base) {
        serial_print("[TASK ERROR] Failed to allocate kernel stack\n");
        kfree(task);
        return NULL;
    }
    
    // Stack'i sıfırla
    memset_task((void*)task->kernel_stack_base, 0, KERNEL_STACK_SIZE);
    
    // CPU context ayarla
    // Stack yukarıdan aşağıya büyür, bu yüzden RSP = base + size
    task->context.rsp = task->kernel_stack_base + KERNEL_STACK_SIZE - 16;
    task->context.rip = (uint64_t)entry_point;
    task->context.rflags = 0x202;  // IF (Interrupt Flag) set
    
    // Segment registers (kernel mode)
    task->context.cs = 0x08;  // Kernel code segment
    task->context.ss = 0x10;  // Kernel stack segment
    task->context.ds = 0x10;
    task->context.es = 0x10;
    
    // CR3 - Şimdilik kernel page table kullan (daha sonra per-process olabilir)
    task->context.cr3 = 0;  // Kernel page table
    
    // Zamanlama bilgileri
    task->time_slice = 10;  // 10 ticks (10ms @ 1000Hz)
    task->time_used = 0;
    task->last_run_time = 0;
    task->context_switches = 0;
    task->total_runtime = 0;
    
    // Debug
    char debug_msg[128];
    debug_msg[0] = '[';
    debug_msg[1] = 'T';
    debug_msg[2] = 'A';
    debug_msg[3] = 'S';
    debug_msg[4] = 'K';
    debug_msg[5] = ']';
    debug_msg[6] = ' ';
    debug_msg[7] = 'C';
    debug_msg[8] = 'r';
    debug_msg[9] = 'e';
    debug_msg[10] = 'a';
    debug_msg[11] = 't';
    debug_msg[12] = 'e';
    debug_msg[13] = 'd';
    debug_msg[14] = ' ';
    debug_msg[15] = 't';
    debug_msg[16] = 'a';
    debug_msg[17] = 's';
    debug_msg[18] = 'k';
    debug_msg[19] = ' ';
    debug_msg[20] = '\'';
    int i = 0;
    while (name[i] && i < 20) {
        debug_msg[21 + i] = name[i];
        i++;
    }
    debug_msg[21 + i] = '\'';
    debug_msg[22 + i] = ' ';
    debug_msg[23 + i] = '(';
    debug_msg[24 + i] = 'P';
    debug_msg[25 + i] = 'I';
    debug_msg[26 + i] = 'D';
    debug_msg[27 + i] = '=';
    char pid_str[16];
    int_to_str(task->pid, pid_str);
    int j = 0;
    while (pid_str[j]) {
        debug_msg[28 + i + j] = pid_str[j];
        j++;
    }
    debug_msg[28 + i + j] = ')';
    debug_msg[29 + i + j] = '\n';
    debug_msg[30 + i + j] = '\0';
    serial_print(debug_msg);
    
    return task;
}

int task_start(task_t* task) {
    if (!task) return -1;
    
    task->state = TASK_STATE_READY;
    task_queue_push(&ready_queue, task);
    
    serial_print("[TASK] Task started and added to ready queue\n");
    return 0;
}

void task_terminate(task_t* task) {
    if (!task) return;
    
    serial_print("[TASK] Terminating task\n");
    
    task->state = TASK_STATE_TERMINATED;
    
    // Queue'dan çıkar
    if (task != current_task) {
        task_queue_remove(&ready_queue, task);
    }
    
    // Belleği temizle (dikkatli!)
    if (task->kernel_stack_base) {
        kfree((void*)task->kernel_stack_base);
    }
    
    // Task yapısını serbest bırak
    kfree(task);
}

void task_exit(void) {
    // Mevcut task kendini sonlandırır
    if (!current_task || current_task == idle_task) {
        serial_print("[TASK ERROR] Cannot exit idle task!\n");
        return;
    }
    
    serial_print("[TASK] Current task exiting - switching immediately\n");
    
    // Mark task as terminated
    current_task->state = TASK_STATE_TERMINATED;
    
    // Get next task to run
    task_t* next = task_get_next();
    if (!next) {
        serial_print("[TASK] No next task, returning to kernel main\n");
        next = idle_task;
    }
    
    serial_print("[TASK] Switching to: ");
    serial_print(next->name);
    serial_print("\n");
    
    // Store old task for scheduler cleanup
    extern task_t* previous_task;
    previous_task = current_task;
    
    // Set new current task
    current_task = next;
    next->state = TASK_STATE_RUNNING;
    next->last_run_time = get_system_ticks();
    
    // Special case: if switching to idle, don't use context switch
    // Just return from this function - we're in a task that's exiting
    // The interrupt will return us to kernel main's HLT loop
    if (next == idle_task) {
        serial_print("[TASK] Returning to kernel main\n");
        // Return to wherever we were called from
        // Since tasks call task_exit() from their own code, we need to
        // jump to a safe return point - kernel main
        extern void kernel_return_point(void);
        // Actually, we can't just return - we need to jump to kernel main
        // For now, just enter the idle loop here
        idle_task_entry();  // This never returns
    }
    
    // For normal tasks, use context switch
    extern void task_load_and_jump_context(cpu_context_t* ctx);
    task_load_and_jump_context(&next->context);
    
    // Should never reach here
    serial_print("[TASK ERROR] task_exit returned - this should never happen!\n");
    while(1) __asm__ volatile("hlt");
}

void task_set_state(task_t* task, uint32_t new_state) {
    if (!task) return;
    task->state = new_state;
}

// ===========================================
// CURRENT TASK MANAGEMENT
// ===========================================

task_t* task_get_current(void) {
    return current_task;
}

task_t* task_get_next(void) {
    // Round-robin: Sıradaki task'ı al
    task_t* next = task_queue_pop(&ready_queue);
    
    if (!next) {
        // Hiç task yoksa idle'a dön
        return idle_task;
    }
    
    return next;
}

uint32_t task_get_count(void) {
    return ready_queue.count;
}

task_t* task_find_by_pid(uint32_t pid) {
    if (current_task && current_task->pid == pid) {
        return current_task;
    }
    
    task_t* task = ready_queue.head;
    while (task) {
        if (task->pid == pid) {
            return task;
        }
        task = task->next;
    }
    
    if (idle_task && idle_task->pid == pid) {
        return idle_task;
    }
    
    return NULL;
}

// ===========================================
// CONTEXT SWITCHING
// ===========================================

// External assembly functions
extern void task_switch_context(cpu_context_t* old_ctx, cpu_context_t* new_ctx);

void task_switch(task_t* from, task_t* to) {
    if (!to) return;
    
    serial_print("[TASK] Switching to task: ");
    serial_print(to->name);
    serial_print("\n");
    
    // İstatistikleri güncelle
    if (from) {
        from->context_switches++;
        from->total_runtime += (get_system_ticks() - from->last_run_time);
    }
    
    to->last_run_time = get_system_ticks();
    to->state = TASK_STATE_RUNNING;
    
    current_task = to;
    
    // Special case: switching TO idle task
    // Don't use context switch - idle doesn't have a valid saved context
    // Just return to kernel main loop which is already in HLT loop
    if (to == idle_task) {
        serial_print("[TASK] Returning to kernel main (idle)\n");
        // Just return - we're already in an interrupt context
        // When we return from the interrupt, we'll be back in kernel main's HLT loop
        return;
    }
    
    // GERÇEK CONTEXT SWITCH - Assembly fonksiyonunu çağır
    cpu_context_t* old_ctx = from ? &from->context : NULL;
    cpu_context_t* new_ctx = &to->context;
    
    task_switch_context(old_ctx, new_ctx);
    
    serial_print("[TASK] Context switch returned\n");
}

// ===========================================
// IDLE TASK
// ===========================================

void idle_task_entry(void) {
    serial_print("[IDLE] Idle task started\n");
    
    while (1) {
        // HLT instruction - CPU'yu bekletir, interrupt gelene kadar
        __asm__ volatile("hlt");
    }
}

task_t* task_create_idle(void) {
    task_t* idle = task_create("idle", idle_task_entry, 0);  // En düşük öncelik
    if (idle) {
        idle->pid = 0;  // Idle task PID = 0
        idle->state = TASK_STATE_RUNNING;  // Hemen çalışıyor
    }
    return idle;
}

// ===========================================
// TEST TASKS
// ===========================================

void test_task_a(void) {
    serial_print("[TASK A] Started\n");
    
    for (int i = 0; i < 5; i++) {
        serial_print("[TASK A] Running iteration ");
        char num[16];
        int_to_str(i, num);
        serial_print(num);
        serial_print("\n");
        
        // Biraz bekle (basit delay loop)
        for (volatile uint64_t j = 0; j < 1000000; j++);
    }
    
    serial_print("[TASK A] Exiting\n");
    task_exit();
}

void test_task_b(void) {
    serial_print("[TASK B] Started\n");
    
    for (int i = 0; i < 5; i++) {
        serial_print("[TASK B] Running iteration ");
        char num[16];
        int_to_str(i, num);
        serial_print(num);
        serial_print("\n");
        
        for (volatile uint64_t j = 0; j < 1000000; j++);
    }
    
    serial_print("[TASK B] Exiting\n");
    task_exit();
}

void test_task_c(void) {
    serial_print("[TASK C] Started\n");
    
    for (int i = 0; i < 5; i++) {
        serial_print("[TASK C] Running iteration ");
        char num[16];
        int_to_str(i, num);
        serial_print(num);
        serial_print("\n");
        
        for (volatile uint64_t j = 0; j < 1000000; j++);
    }
    
    serial_print("[TASK C] Exiting\n");
    task_exit();
}

void offihito_task(void) {
    serial_print("[OFFIHITO] Task started\n");
    
    // Simple counter - print every N iterations
    uint64_t counter = 0;
    const uint64_t PRINT_EVERY = 100000; // Adjust this value
    
    while (1) {
        counter++;
        
        // Print every PRINT_EVERY iterations
        if (counter >= PRINT_EVERY) {
            // Direct VGA write to ensure it works
            extern void println64(const char* str, uint8_t color);
            println64("Offihito", 0x0D); // Magenta (0x05) or Light Magenta (0x0D)
            serial_print("[OFFIHITO] Printed to screen\n");
            counter = 0; // Reset counter
        }
        
        // Small yield - give other tasks a chance
        // Instead of hlt, use a very small delay
        for (volatile int i = 0; i < 1000; i++);
    }
}

// ===========================================
// UTILITY FUNCTIONS
// ===========================================

void task_print_info(task_t* task) {
    if (!task) return;
    
    #ifdef TEXT_MODE
    print_str64("Task '", VGA_CYAN);
    print_str64(task->name, VGA_YELLOW);
    print_str64("' (PID=", VGA_CYAN);
    char pid_str[16];
    int_to_str(task->pid, pid_str);
    print_str64(pid_str, VGA_WHITE);
    print_str64(", State=", VGA_CYAN);
    
    const char* state_str;
    switch (task->state) {
        case TASK_STATE_READY: state_str = "READY"; break;
        case TASK_STATE_RUNNING: state_str = "RUNNING"; break;
        case TASK_STATE_BLOCKED: state_str = "BLOCKED"; break;
        case TASK_STATE_TERMINATED: state_str = "TERMINATED"; break;
        default: state_str = "UNKNOWN"; break;
    }
    print_str64(state_str, VGA_GREEN);
    println64(")", VGA_CYAN);
    #endif
}

void task_list_all(void) {
    #ifdef TEXT_MODE
    println64("=== Task List ===", VGA_CYAN);
    
    // Current task
    if (current_task) {
        print_str64("* CURRENT: ", VGA_GREEN);
        task_print_info(current_task);
    }
    
    // Ready queue
    println64("READY QUEUE:", VGA_YELLOW);
    task_t* task = ready_queue.head;
    while (task) {
        print_str64("  - ", VGA_WHITE);
        task_print_info(task);
        task = task->next;
    }
    
    // Idle task
    if (idle_task) {
        print_str64("IDLE: ", VGA_CYAN);
        task_print_info(idle_task);
    }
    #endif
}

void task_print_stats(void) {
    #ifdef TEXT_MODE
    println64("=== Task Statistics ===", VGA_CYAN);
    
    char str[64];
    str_cpy(str, "Total tasks: ");
    char num[16];
    int_to_str(ready_queue.count + (current_task ? 1 : 0), num);
    str_concat(str, num);
    println64(str, VGA_WHITE);
    
    str_cpy(str, "Next PID: ");
    int_to_str(next_pid, num);
    str_concat(str, num);
    println64(str, VGA_WHITE);
    #endif
}