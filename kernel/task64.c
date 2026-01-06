// task64.c - AscentOS Multitasking Implementation
#include "task64.h"

// External functions
extern void* kmalloc(size_t size);
extern void kfree(void* ptr);
extern void println64(const char* str, uint8_t color);
extern void print_str64(const char* str, uint8_t color);
extern void serial_print(const char* str);

// Task list
static Task* task_list = NULL;
static Task* current_task = NULL;
static uint32_t next_pid = 1;
static uint64_t system_ticks = 0;

// Idle task
static Task idle_task;
static uint8_t idle_stack[TASK_STACK_SIZE];

// String utilities
static int str_len(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len;
}

static void str_cpy(char* dest, const char* src) {
    while (*src) *dest++ = *src++;
    *dest = '\0';
}

// Get system tick count
uint64_t get_system_ticks(void) {
    return system_ticks;
}

void task_increment_ticks(void) {
    system_ticks++;
}

// Initialize task system
void task_init(void) {
    serial_print("Initializing multitasking system...\n");
    
    // Create idle task
    idle_task.pid = 0;
    str_cpy(idle_task.name, "idle");
    idle_task.state = TASK_STATE_READY;
    idle_task.priority = 0;  // Lowest priority
    idle_task.stack = idle_stack;
    idle_task.cpu_time = 0;
    idle_task.start_time = 0;
    idle_task.parent_pid = 0;
    idle_task.next = NULL;
    
    // Initialize idle task context
    idle_task.context.rsp = (uint64_t)(idle_stack + TASK_STACK_SIZE - 16);
    idle_task.context.rbp = idle_task.context.rsp;
    idle_task.context.rip = 0;  // Idle just halts
    idle_task.context.rflags = 0x202;  // IF flag set
    idle_task.context.cs = 0x08;
    idle_task.context.ss = 0x10;
    
    // Idle is always first
    task_list = &idle_task;
    current_task = &idle_task;
    
    serial_print("Multitasking initialized!\n");
}

// Create new task
Task* task_create(const char* name, void (*entry_point)(void), uint8_t priority) {
    // Allocate task structure
    Task* task = (Task*)kmalloc(sizeof(Task));
    if (!task) {
        serial_print("Failed to allocate task structure\n");
        return NULL;
    }
    
    // Allocate stack
    task->stack = (uint8_t*)kmalloc(TASK_STACK_SIZE);
    if (!task->stack) {
        serial_print("Failed to allocate task stack\n");
        kfree(task);
        return NULL;
    }
    
    // Initialize task
    task->pid = next_pid++;
    str_cpy(task->name, name);
    task->state = TASK_STATE_READY;
    task->priority = priority;
    task->cpu_time = 0;
    task->start_time = system_ticks;
    task->sleep_until = 0;
    task->parent_pid = current_task ? current_task->pid : 0;
    task->exit_code = 0;
    
    // Setup initial stack frame for task entry
    uint64_t* stack_ptr = (uint64_t*)(task->stack + TASK_STACK_SIZE);
    
    // Push initial values (for iretq)
    *(--stack_ptr) = 0x10;              // SS
    *(--stack_ptr) = (uint64_t)stack_ptr; // RSP (will be updated)
    *(--stack_ptr) = 0x202;             // RFLAGS (IF set)
    *(--stack_ptr) = 0x08;              // CS
    *(--stack_ptr) = (uint64_t)entry_point; // RIP
    
    // Push general purpose registers (will be loaded by task_switch)
    *(--stack_ptr) = 0;  // RAX
    *(--stack_ptr) = 0;  // RBX
    *(--stack_ptr) = 0;  // RCX
    *(--stack_ptr) = 0;  // RDX
    *(--stack_ptr) = 0;  // RSI
    *(--stack_ptr) = 0;  // RDI
    *(--stack_ptr) = (uint64_t)stack_ptr + 8;  // RBP
    *(--stack_ptr) = 0;  // R8
    *(--stack_ptr) = 0;  // R9
    *(--stack_ptr) = 0;  // R10
    *(--stack_ptr) = 0;  // R11
    *(--stack_ptr) = 0;  // R12
    *(--stack_ptr) = 0;  // R13
    *(--stack_ptr) = 0;  // R14
    *(--stack_ptr) = 0;  // R15
    
    // Set context
    task->context.rsp = (uint64_t)stack_ptr;
    task->context.rbp = task->context.rsp;
    task->context.rip = (uint64_t)entry_point;
    task->context.rflags = 0x202;
    task->context.cs = 0x08;
    task->context.ss = 0x10;
    
    // Add to task list
    task->next = task_list;
    task_list = task;
    
    char msg[64];
    str_cpy(msg, "Task created: ");
    int len = str_len(msg);
    str_cpy(msg + len, name);
    len = str_len(msg);
    msg[len] = '\n';
    msg[len + 1] = '\0';
    serial_print(msg);
    
    return task;
}

// Terminate task
void task_terminate(Task* task, int exit_code) {
    if (!task) return;
    
    task->state = TASK_STATE_TERMINATED;
    task->exit_code = exit_code;
    
    char msg[64];
    str_cpy(msg, "Task terminated: ");
    int len = str_len(msg);
    str_cpy(msg + len, task->name);
    len = str_len(msg);
    msg[len] = '\n';
    msg[len + 1] = '\0';
    serial_print(msg);
    
    // If current task is terminating, switch to another
    if (task == current_task) {
        task_yield();
    }
}

// Sleep for milliseconds
void task_sleep(uint64_t milliseconds) {
    if (!current_task) return;
    
    current_task->state = TASK_STATE_SLEEPING;
    current_task->sleep_until = system_ticks + milliseconds;
    
    task_yield();
}

// Yield CPU to another task
void task_yield(void) {
    // This will be called by scheduler interrupt
    __asm__ volatile("int $0x80");
}

// Get current task
Task* task_get_current(void) {
    return current_task;
}

// Get task by PID
Task* task_get_by_pid(uint32_t pid) {
    Task* task = task_list;
    while (task) {
        if (task->pid == pid) return task;
        task = task->next;
    }
    return NULL;
}

// Get all tasks
int task_get_all(Task** tasks, int max_tasks) {
    int count = 0;
    Task* task = task_list;
    
    while (task && count < max_tasks) {
        tasks[count++] = task;
        task = task->next;
    }
    
    return count;
}

// Count tasks
int task_count(void) {
    int count = 0;
    Task* task = task_list;
    
    while (task) {
        if (task->state != TASK_STATE_TERMINATED) {
            count++;
        }
        task = task->next;
    }
    
    return count;
}

// Initialize scheduler
void scheduler_init(void) {
    task_init();
    serial_print("Scheduler initialized\n");
}

// Scheduler tick (called by timer interrupt)
void scheduler_tick(void) {
    system_ticks++;
    
    if (!current_task) return;
    
    // Update current task CPU time
    current_task->cpu_time++;
    
    // Wake up sleeping tasks
    Task* task = task_list;
    while (task) {
        if (task->state == TASK_STATE_SLEEPING) {
            if (system_ticks >= task->sleep_until) {
                task->state = TASK_STATE_READY;
            }
        }
        task = task->next;
    }
}

// Select next task to run (Round-robin with priority)
Task* scheduler_select_next(void) {
    if (!current_task) return &idle_task;
    
    // Start from next task after current
    Task* task = current_task->next;
    if (!task) task = task_list;
    
    // Find highest priority ready task
    Task* best = NULL;
    uint8_t best_priority = 0;
    
    Task* start = task;
    do {
        if (task->state == TASK_STATE_READY) {
            if (!best || task->priority > best_priority) {
                best = task;
                best_priority = task->priority;
            }
        }
        
        task = task->next;
        if (!task) task = task_list;
    } while (task != start);
    
    // If no ready task, return idle
    if (!best) return &idle_task;
    
    return best;
}

// Get CPU usage percentage for task
uint32_t task_get_cpu_usage(Task* task) {
    if (!task || system_ticks == 0) return 0;
    
    uint64_t uptime = system_ticks - task->start_time;
    if (uptime == 0) return 0;
    
    return (task->cpu_time * 100) / uptime;
}

// Get task uptime in milliseconds
uint64_t task_get_uptime(Task* task) {
    if (!task) return 0;
    return system_ticks - task->start_time;
}

// ===========================================
// DEMO TASKS
// ===========================================

// Counter task - counts from 0 upwards
void demo_task_counter(void) {
    uint64_t counter = 0;
    
    while (1) {
        counter++;
        
        // Every 1000 iterations, print
        if (counter % 10000 == 0) {
            // Just loop - we can't print from here safely yet
        }
        
        // Sleep a bit
        for (volatile int i = 0; i < 10000; i++);
        
        task_yield();
    }
}

// Spinner task - shows activity
void demo_task_spinner(void) {
    const char spinners[] = "|/-\\";
    int index = 0;
    
    while (1) {
        index = (index + 1) % 4;
        
        // Sleep
        for (volatile int i = 0; i < 50000; i++);
        
        task_yield();
    }
}

// Calculator task - does math
void demo_task_calculator(void) {
    uint64_t result = 0;
    
    while (1) {
        // Do some calculations
        for (int i = 0; i < 1000; i++) {
            result = result * 13 + i;
            result = result % 999999;
        }
        
        task_yield();
    }
}