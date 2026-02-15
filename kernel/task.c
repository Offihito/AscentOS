// task.c - Task Management Implementation
// PHASE 2: WITH USERMODE SUPPORT
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
    task_system_initialized = 1;
    
    // Idle task oluştur
    idle_task = task_create_idle();
    if (idle_task) {
        serial_print("[TASK] Idle task created (PID=0)\n");
    } else {
        serial_print("[TASK ERROR] Failed to create idle task!\n");
        task_system_initialized = 0;
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
    task->privilege_level = TASK_PRIVILEGE_KERNEL;  // Kernel mode task
    
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
    
    // Setup stack with IRET frame for first run
    uint64_t* stack_top = (uint64_t*)(task->kernel_stack_base + KERNEL_STACK_SIZE);
    
    serial_print("[TASK] Stack base: 0x");
    char addr_str[20];
    uint64_to_string(task->kernel_stack_base, addr_str);
    serial_print(addr_str);
    serial_print("\n");
    
    serial_print("[TASK] Stack top: 0x");
    uint64_to_string((uint64_t)stack_top, addr_str);
    serial_print(addr_str);
    serial_print("\n");
    
    // Build IRET frame
    *(--stack_top) = 0x10;  // SS
    *(--stack_top) = (uint64_t)(stack_top - 10);  // RSP
    *(--stack_top) = 0x202; // RFLAGS
    *(--stack_top) = 0x08;  // CS
    *(--stack_top) = (uint64_t)entry_point;  // RIP
    
    serial_print("[TASK] Entry point: 0x");
    uint64_to_string((uint64_t)entry_point, addr_str);
    serial_print(addr_str);
    serial_print("\n");
    
    // Add space for 15 registers (r15-rax)
    for (int i = 0; i < 15; i++) {
        *(--stack_top) = 0;
    }
    
    serial_print("[TASK] Stack prepared at: 0x");
    uint64_to_string((uint64_t)stack_top, addr_str);
    serial_print(addr_str);
    serial_print("\n");
    
    // CPU context setup
    task->context.rsp = (uint64_t)stack_top;
    task->context.rip = (uint64_t)entry_point;
    task->context.rflags = 0x202;
    
    // Segment registers
    task->context.cs = 0x08;
    task->context.ss = 0x10;
    task->context.ds = 0x10;
    task->context.es = 0x10;
    
    // CR3 - Kernel page table
    task->context.cr3 = 0;
    
    // Zamanlama bilgileri
    task->time_slice = 10;
    task->time_used = 0;
    task->last_run_time = 0;
    task->context_switches = 0;
    task->total_runtime = 0;
    
    // Debug
    serial_print("[TASK] Created kernel task '");
    serial_print(name);
    serial_print("' (PID=");
    char pid_str[16];
    int_to_str(task->pid, pid_str);
    serial_print(pid_str);
    serial_print(")\n");
    
    return task;
}

// ===========================================
// USERMODE TASK CREATION - NEW FOR PHASE 2
// ===========================================

task_t* task_create_user(const char* name, void (*entry_point)(void), uint32_t priority) {
    if (!task_system_initialized) {
        serial_print("[TASK ERROR] Task system not initialized!\n");
        return NULL;
    }
    
    serial_print("[TASK] Creating usermode task '");
    serial_print(name);
    serial_print("'...\n");
    
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
    task->privilege_level = TASK_PRIVILEGE_USER;  // USER MODE!
    
    // Kernel stack ayır (syscall/interrupt için gerekli)
    task->kernel_stack_size = KERNEL_STACK_SIZE;
    task->kernel_stack_base = (uint64_t)kmalloc(KERNEL_STACK_SIZE);
    if (!task->kernel_stack_base) {
        serial_print("[TASK ERROR] Failed to allocate kernel stack\n");
        kfree(task);
        return NULL;
    }
    memset_task((void*)task->kernel_stack_base, 0, KERNEL_STACK_SIZE);
    
    // User stack ayır (usermode execution için)
    task->user_stack_size = USER_STACK_SIZE;
    task->user_stack_base = (uint64_t)kmalloc(USER_STACK_SIZE);
    if (!task->user_stack_base) {
        serial_print("[TASK ERROR] Failed to allocate user stack\n");
        kfree((void*)task->kernel_stack_base);
        kfree(task);
        return NULL;
    }
    memset_task((void*)task->user_stack_base, 0, USER_STACK_SIZE);
    
    serial_print("[TASK] User stack allocated at: 0x");
    char addr_str[20];
    uint64_to_string(task->user_stack_base, addr_str);
    serial_print(addr_str);
    serial_print("\n");
    
    // Setup USER stack with IRET frame for first run
    // Usermode tasks return via IRETQ which expects specific stack layout
    uint64_t* stack_top = (uint64_t*)(task->user_stack_base + USER_STACK_SIZE);
    
    // Build IRET frame (from high to low address)
    *(--stack_top) = 0x23;  // SS (User DS)
    *(--stack_top) = (uint64_t)(stack_top - 10);  // RSP (will adjust)
    *(--stack_top) = 0x202; // RFLAGS (IF set)
    *(--stack_top) = 0x1B;  // CS (User CS)
    *(--stack_top) = (uint64_t)entry_point;  // RIP
    
    serial_print("[TASK] User entry point: 0x");
    uint64_to_string((uint64_t)entry_point, addr_str);
    serial_print(addr_str);
    serial_print("\n");
    
    // Add space for 15 registers (will be popped by interrupt handler)
    for (int i = 0; i < 15; i++) {
        *(--stack_top) = 0;
    }
    
    serial_print("[TASK] User stack prepared at: 0x");
    uint64_to_string((uint64_t)stack_top, addr_str);
    serial_print(addr_str);
    serial_print("\n");
    
    // CPU context ayarla - USERMODE!
    // RSP points to prepared stack (not user_stack_base!)
    task->context.rsp = (uint64_t)stack_top;  // Point to prepared stack
    task->context.rip = (uint64_t)entry_point;
    task->context.rflags = 0x202;
    
    // Segment registers (USER mode - Ring 3)
    task->context.cs = 0x1B;  // User code segment
    task->context.ss = 0x23;  // User data segment
    task->context.ds = 0x23;
    task->context.es = 0x23;
    
    // CR3 - Kernel page table (for now, later can be per-process)
    task->context.cr3 = 0;
    
    // Zamanlama bilgileri
    task->time_slice = 10;
    task->time_used = 0;
    task->last_run_time = 0;
    task->context_switches = 0;
    task->total_runtime = 0;
    
    serial_print("[TASK] Created usermode task '");
    serial_print(name);
    serial_print("' (PID=");
    char pid_str[16];
    int_to_str(task->pid, pid_str);
    serial_print(pid_str);
    serial_print(") Ring 3\n");
    
    return task;
}

// ===========================================
// TASK LIFECYCLE
// ===========================================

int task_start(task_t* task) {
    if (!task) return -1;
    
    // Only log if this is the first time (context_switches == 0)
    if (task->context_switches == 0) {
        serial_print("[TASK] Starting task '");
        serial_print(task->name);
        serial_print("'\n");
    }
    
    task->state = TASK_STATE_READY;
    task_queue_push(&ready_queue, task);
    
    return 0;
}

void task_terminate(task_t* task) {
    if (!task) return;
    
    serial_print("[TASK] Terminating task '");
    serial_print(task->name);
    serial_print("'\n");
    
    task->state = TASK_STATE_TERMINATED;
    
    // Remove from queue if present
    task_queue_remove(&ready_queue, task);
    
    // Belleği temizle
    if (task->kernel_stack_base) {
        kfree((void*)task->kernel_stack_base);
    }
    if (task->user_stack_base) {
        kfree((void*)task->user_stack_base);
    }
    
    kfree(task);
}

void task_exit(void) {
    if (!current_task || current_task == idle_task) {
        serial_print("[TASK ERROR] Cannot exit idle task!\n");
        return;
    }
    
    serial_print("[TASK] Current task '");
    serial_print(current_task->name);
    serial_print("' exiting\n");
    
    // Mark as terminated
    current_task->state = TASK_STATE_TERMINATED;
    
    // Get next task
    task_t* next = task_get_next();
    if (!next) {
        next = idle_task;
    }
    
    // Store for cleanup
    extern task_t* previous_task;
    previous_task = current_task;
    
    // Switch
    current_task = next;
    next->state = TASK_STATE_RUNNING;
    next->last_run_time = get_system_ticks();
    
    // Special case: idle
    if (next == idle_task) {
        serial_print("[TASK] Returning to idle\n");
        idle_task_entry();
    }
    
    // Load context
    extern void task_load_and_jump_context(cpu_context_t* ctx);
    task_load_and_jump_context(&next->context);
    
    // Should never reach
    serial_print("[TASK ERROR] task_exit returned!\n");
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

void task_set_current(task_t* task) {
    if (!task) return;
    current_task = task;
    task->last_run_time = get_system_ticks();
}

task_t* task_get_next(void) {
    task_t* next = task_queue_pop(&ready_queue);
    if (!next) {
        return idle_task;
    }
    
    serial_print("[TASK] Getting next: ");
    serial_print(next->name);
    serial_print(" (context_switches=");
    char num[16];
    int_to_str((int)next->context_switches, num);
    serial_print(num);
    serial_print(", RSP=0x");
    char addr[20];
    uint64_to_string(next->context.rsp, addr);
    serial_print(addr);
    serial_print(")\n");
    
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

void task_save_context(cpu_context_t* context) {
    // Assembly fonksiyonunu çağır
    extern void task_save_current_context(cpu_context_t* ctx);
    task_save_current_context(context);
}

void task_load_context(cpu_context_t* context) {
    // Assembly fonksiyonunu çağır
    extern void task_load_and_jump_context(cpu_context_t* ctx);
    task_load_and_jump_context(context);
}

void task_switch(task_t* from, task_t* to) {
    if (!to) {
        serial_print("[TASK ERROR] Cannot switch to NULL task!\n");
        return;
    }
    
    serial_print("[TASK] Switching from '");
    serial_print(from ? from->name : "NULL");
    serial_print("' to '");
    serial_print(to->name);
    serial_print("'\n");
    
    // Update current task
    current_task = to;
    to->state = TASK_STATE_RUNNING;
    to->last_run_time = get_system_ticks();
    to->context_switches++;
    
    // Special case: first task or switching to idle
    if (!from || from == idle_task) {
        serial_print("[TASK] Jumping to task\n");
        extern void task_load_and_jump_context(cpu_context_t* ctx);
        task_load_and_jump_context(&to->context);
        serial_print("[TASK] Returning to idle\n");
        return;
    }
    
    // Ring 3 (usermode) task: use IRET-based privilege transition
    // jump_to_usermode builds a proper IRET frame (SS/RSP/RFLAGS/CS/RIP)
    // and drops to Ring 3. It never returns — the task must exit via syscall.
    if (to->privilege_level == TASK_PRIVILEGE_USER) {
        serial_print("[TASK] Transitioning to Ring 3 via IRET\n");
        jump_to_usermode(to->context.rip, to->context.rsp);
        // Never reached
        serial_print("[TASK ERROR] jump_to_usermode returned!\n");
        return;
    }
    
    // Ring 0 (kernel) task: normal context switch via assembly
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
        __asm__ volatile("hlt");
    }
}

task_t* task_create_idle(void) {
    task_t* idle = task_create("idle", idle_task_entry, 0);
    if (idle) {
        idle->pid = 0;
        idle->state = TASK_STATE_RUNNING;
    }
    return idle;
}

// ===========================================
// TEST TASKS - KERNEL MODE
// ===========================================

void test_task_a(void) {
    serial_print("[TASK A] Started (Ring 0)\n");
    
    for (int i = 0; i < 5; i++) {
        serial_print("[TASK A] Iteration ");
        char num[16];
        int_to_str(i, num);
        serial_print(num);
        serial_print("\n");
        
        for (volatile uint64_t j = 0; j < 1000000; j++);
    }
    
    serial_print("[TASK A] Exiting\n");
    task_exit();
}

void test_task_b(void) {
    serial_print("[TASK B] Started (Ring 0)\n");
    
    for (int i = 0; i < 5; i++) {
        serial_print("[TASK B] Iteration ");
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
    serial_print("[TASK C] Started (Ring 0)\n");
    
    for (int i = 0; i < 5; i++) {
        serial_print("[TASK C] Iteration ");
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
    
    uint64_t counter = 0;
    const uint64_t PRINT_EVERY = 100000;
    
    while (1) {
        counter++;
        
        if (counter >= PRINT_EVERY) {
            extern void println64(const char* str, uint8_t color);
            println64("Offihito", 0x0D);
            serial_print("[OFFIHITO] Printed\n");
            counter = 0;
        }
        
        for (volatile int i = 0; i < 1000; i++);
    }
}

// ===========================================
// TEST TASKS - USER MODE (PHASE 2)
// ===========================================

// Simple usermode task that just loops
void usermode_test_task(void) {
    // This will run in Ring 3!
    // We can't use serial_print directly (it's a kernel function)
    // We need to use syscalls instead
    
    // For now, just loop and exit
    // Later we'll add syscalls
    
    volatile int counter = 0;
    for (int i = 0; i < 1000000; i++) {
        counter++;
    }
    
    // Exit via syscall (when implemented)
    // For now, just infinite loop
    while(1) {
        __asm__ volatile("hlt");
    }
}

// Usermode task that tests syscalls AND reports its own ring level
void usermode_syscall_task(void) {
    // ── We are now in Ring 3 ─────────────────────────────────────────────────
    // Cannot call kernel functions directly (serial_print, etc.)
    // Must use syscalls for everything.

    // Report: "Hello from usermode!" via sys_ascent_debug (SYS 300)
    // This syscall handler is in kernel; it will print to serial.
    const char* hello = "=== usermode_syscall_task: running in Ring 3! ===";
    __asm__ volatile (
        "movq $300, %%rax\n\t"   // SYS_ASCENT_DEBUG
        "movq %0,   %%rdi\n\t"
        "syscall\n\t"
        :: "r"(hello)
        : "rax", "rdi", "rcx", "r11", "memory"
    );

    // Get our PID via sys_getpid (SYS 39)
    int64_t pid;
    __asm__ volatile (
        "movq $39, %%rax\n\t"    // SYS_GETPID
        "syscall\n\t"
        "movq %%rax, %0\n\t"
        : "=r"(pid)
        :: "rax", "rcx", "r11", "memory"
    );
    (void)pid;  // used implicitly

    // Print a second debug message confirming syscall round-trip
    const char* ok = "usermode_syscall_task: sys_getpid() syscall returned OK";
    __asm__ volatile (
        "movq $300, %%rax\n\t"
        "movq %0,   %%rdi\n\t"
        "syscall\n\t"
        :: "r"(ok)
        : "rax", "rdi", "rcx", "r11", "memory"
    );

    // Write to stdout (fd=1) via sys_write (SYS 1)
    const char* out = "Ring3: write to stdout via syscall\n";
    int64_t len = 36;
    __asm__ volatile (
        "movq $1,  %%rax\n\t"    // SYS_WRITE
        "movq $1,  %%rdi\n\t"    // fd = 1 (stdout)
        "movq %0,  %%rsi\n\t"    // buf
        "movq %1,  %%rdx\n\t"    // count
        "syscall\n\t"
        :: "r"(out), "r"(len)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );

    // Exit cleanly via sys_exit (SYS 60)
    __asm__ volatile (
        "movq $60, %%rax\n\t"    // SYS_EXIT
        "xorq %%rdi, %%rdi\n\t"  // status = 0
        "syscall\n\t"
        ::: "rax", "rdi", "rcx", "r11", "memory"
    );

    // Unreachable — safety halt
    while (1) __asm__ volatile("hlt");
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
    
    print_str64(", Ring ", VGA_CYAN);
    char ring[2];
    ring[0] = '0' + task->privilege_level;
    ring[1] = '\0';
    print_str64(ring, VGA_WHITE);
    
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
    
    if (current_task) {
        print_str64("* CURRENT: ", VGA_GREEN);
        task_print_info(current_task);
    }
    
    println64("READY QUEUE:", VGA_YELLOW);
    task_t* task = ready_queue.head;
    while (task) {
        print_str64("  - ", VGA_WHITE);
        task_print_info(task);
        task = task->next;
    }
    
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
    char num[16];
    
    serial_print("Total tasks: ");
    int_to_str(ready_queue.count + (current_task ? 1 : 0), num);
    serial_print(num);
    serial_print("\n");
    
    serial_print("Next PID: ");
    int_to_str(next_pid, num);
    serial_print(num);
    serial_print("\n");
    #endif
}