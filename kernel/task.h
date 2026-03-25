#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "syscall.h"   
#include "signal64.h"   
#include "elf64.h"      


// Task States
#define TASK_STATE_READY      0
#define TASK_STATE_RUNNING    1
#define TASK_STATE_BLOCKED    2
#define TASK_STATE_TERMINATED 3
#define TASK_STATE_SLEEPING   4
#define TASK_STATE_ZOMBIE     5
#define TASK_STATE_STOPPED    6

#define TASK_PRIVILEGE_KERNEL   0
#define TASK_PRIVILEGE_USER     3

#define KERNEL_STACK_SIZE  0x200000   // 2 MB 
#define USER_STACK_SIZE    0x800000   // 8 MB 

// Priority Limits
#define TASK_PRIORITY_IDLE     0
#define TASK_PRIORITY_LOW      32
#define TASK_PRIORITY_NORMAL   128
#define TASK_PRIORITY_HIGH     200
#define TASK_PRIORITY_REALTIME 255

// TSS (Task State Segment) - 64-bit
typedef struct __attribute__((packed)) {
    uint32_t reserved0;     // +0x00
    uint64_t rsp0;          // +0x04  
    uint64_t rsp1;          // +0x0C 
    uint64_t rsp2;          // +0x14  
    uint64_t reserved1;     // +0x1C
    uint64_t ist1;          // +0x24  
    uint64_t ist2;          // +0x2C
    uint64_t ist3;          // +0x34
    uint64_t ist4;          // +0x3C
    uint64_t ist5;          // +0x44
    uint64_t ist6;          // +0x4C
    uint64_t ist7;          // +0x54
    uint64_t reserved2;     // +0x5C
    uint16_t reserved3;     // +0x64
    uint16_t iopb_offset;   // +0x66  
} tss_t;                    // Total = 104 bytes

extern tss_t kernel_tss;

// GDT Selectors
#define GDT_KERNEL_CODE      0x08
#define GDT_KERNEL_DATA      0x10
#define GDT_USER_DATA        0x18
#define GDT_USER_CODE        0x20
#define GDT_TSS_SELECTOR     0x28

#define GDT_USER_DATA_RPL3   (GDT_USER_DATA | 3)   // 0x1B
#define GDT_USER_CODE_RPL3   (GDT_USER_CODE | 3)   // 0x23

// CPU Context
typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8,  r9,  r10, r11, r12, r13, r14, r15;
    uint64_t rip;
    uint64_t rflags;
    uint64_t cs, ss, ds, es, fs, gs;
    uint64_t cr3;
} cpu_context_t;

// Task Control Block (TCB)
#define TASK_HAS_PARENT_PID

typedef struct task {
    // Identity 
    uint32_t pid;              
    uint32_t parent_pid;     
    uint32_t pgid;           
    uint32_t sid;               
    char     name[32];         

    // -- State -----------------------------------------------
    uint32_t state;             // TASK_STATE_*
    uint32_t privilege_level;   // 0=kernel, 3=user
    int32_t  exit_code;         // Exit code from SYS_EXIT

    // -- Scheduling ------------------------------------------
    uint32_t priority;          // 0-255; higher = more priority
    uint64_t time_slice;        // Time slice in ticks
    uint64_t time_used;         // Ticks consumed from current slice
    uint64_t last_run_time;     // Last run timestamp (ticks)
    uint64_t wake_tick;         // Wake-up tick for TASK_STATE_SLEEPING

    // -- CPU State -------------------------------------------
    cpu_context_t context;

    // -- Kernel Stack ----------------------------------------
    uint64_t kernel_stack_base;
    uint64_t kernel_stack_top;
    uint64_t kernel_stack_size;

    // -- User Stack ------------------------------------------
    uint64_t user_stack_base;
    uint64_t user_stack_top;
    uint64_t user_stack_size;

    // -- File Descriptor Table -------------------------------
    fd_entry_t fd_table[MAX_FDS];

    // -- Signal Infrastructure -------------------------------
    signal_table_t  signal_table;
    syscall_frame_t signal_saved_frame;
    uint64_t        signal_trampoline;

    // -- Scheduler Links -------------------------------------
    struct task* next;
    struct task* prev;

    // -- Statistics ------------------------------------------
    uint64_t context_switches;
    uint64_t total_ticks;

    // -- TLS (Thread Local Storage) --------------------------
    uint64_t fs_base;           // MSR_FS_BASE value
    uint64_t gs_base;           // MSR_GS_BASE value

    // -- ELF Load Range --------------------------------------
    uint64_t elf_load_min;
    uint64_t elf_load_max;

    // -- Per-process user heap (brk) -------------------------
    uint64_t user_brk;
} task_t;

typedef struct {
    task_t*  head;
    task_t*  tail;
    uint32_t count;
} task_queue_t;

// Foreground Task Management
extern volatile uint32_t foreground_pid;
extern void shell_restore_prompt(void);

// TSS Management
void tss_init(void);

static inline void tss_set_kernel_stack(uint64_t rsp0) {
    kernel_tss.rsp0 = rsp0;
}

void gdt_install_user_segments(void);

// Task Management
void task_init(void);

task_t* task_create(const char* name,
                    void (*entry_point)(void),
                    uint32_t priority);

task_t* task_create_user(const char* name,
                         void (*entry_point)(void),
                         uint32_t priority);

task_t* task_create_from_elf(const char* name,
                              const ElfImage* img,
                              uint32_t priority,
                              int argc,
                              const char** argv);

int  task_start(task_t* task);
void task_terminate(task_t* task);
void task_exit(void);
void task_set_state(task_t* task, uint32_t new_state);
void task_reap_zombie(task_t* task);

void task_sleep(task_t* task, uint64_t ticks);
void task_wakeup_check(void);

// Queue Operations
void    task_queue_init(task_queue_t* queue);
void    task_queue_push(task_queue_t* queue, task_t* task);
task_t* task_queue_pop(task_queue_t* queue);
void    task_queue_remove(task_queue_t* queue, task_t* task);
int     task_queue_is_empty(task_queue_t* queue);

// Current Task Management
task_t*  task_get_current(void);
void     task_set_current(task_t* task);
task_t*  task_get_next(void);
uint32_t task_get_count(void);
task_t*  task_find_by_pid(uint32_t pid);

static inline task_t* task_get_by_pid(int pid) {
    return (pid >= 0) ? task_find_by_pid((uint32_t)pid) : (task_t*)0;
}

static inline signal_table_t* task_get_signal_table(task_t* t) {
    return t ? &t->signal_table : (signal_table_t*)0;
}

static inline syscall_frame_t* task_get_saved_frame(task_t* t) {
    return t ? &t->signal_saved_frame : (syscall_frame_t*)0;
}

static inline uint64_t task_get_trampoline(task_t* t) {
    return t ? t->signal_trampoline : 0;
}

void task_set_stopped(task_t* t, int stopped);

// Process Group & Session Management
static inline int task_get_pgid(task_t* t) {
    return t ? (int)t->pgid : -1;
}

static inline int task_get_sid(task_t* t) {
    return t ? (int)t->sid : -1;
}

int task_set_pgid(task_t* t, uint32_t new_pgid);
int task_do_setsid(task_t* t);

// Context Switch
void task_save_context(cpu_context_t* context);
void task_load_context(cpu_context_t* context);
void task_switch(task_t* from, task_t* to);
void task_save_fs_base(void);
void task_restore_fs_base(void);

extern void task_switch_context(cpu_context_t* old_ctx, cpu_context_t* new_ctx);
extern void task_save_current_context(cpu_context_t* ctx);
extern void task_load_and_jump_context(cpu_context_t* ctx);

void task_print_info(task_t* task);
void task_list_all(void);
void task_print_stats(void);
const char* task_state_name(uint32_t state);

// Idle & Test Tasks
void    idle_task_entry(void);
task_t* task_create_idle(void);

void test_task_a(void);
void test_task_b(void);
void test_task_c(void);
void offihito_task(void);
void user_mode_test_task(void);

#endif 