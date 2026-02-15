// syscall.c - System Call Implementation for AscentOS
#include "syscall.h"
#include "task.h"
#include "scheduler.h"

// External functions
extern void serial_print(const char* str);
extern void int_to_str(int num, char* str);
extern uint64_t get_system_ticks(void);

// Helper function (defined at top to avoid forward declaration issues)
static void uint64_to_string(uint64_t num, char* str) {
    if (num == 0) {
        str[0] = '0';
        str[1] = '\0';
        return;
    }
    
    char temp[32];
    int i = 0;
    
    while (num > 0) {
        temp[i++] = '0' + (num % 10);
        num /= 10;
    }
    
    // Reverse
    for (int j = 0; j < i; j++) {
        str[j] = temp[i - j - 1];
    }
    str[i] = '\0';
}

// ===========================================
// GLOBAL VARIABLES
// ===========================================

static int syscall_enabled = 0;
static syscall_stats_t stats;

// ===========================================
// SYSCALL INITIALIZATION
// ===========================================

void syscall_init(void) {
    if (syscall_enabled) {
        serial_print("[SYSCALL] Already initialized\n");
        return;
    }
    
    serial_print("[SYSCALL] Initializing syscall system...\n");
    
    // Reset statistics
    stats.total_syscalls = 0;
    stats.invalid_syscalls = 0;
    stats.failed_syscalls = 0;
    for (int i = 0; i < SYSCALL_MAX; i++) {
        stats.syscall_counts[i] = 0;
    }
    
    // Setup MSRs (done in syscall_setup.c)
    extern void syscall_setup_msrs(void);
    syscall_setup_msrs();
    
    syscall_enabled = 1;
    serial_print("[SYSCALL] Syscall system initialized\n");
}

int syscall_is_enabled(void) {
    return syscall_enabled;
}

// ===========================================
// SYSCALL DISPATCHER
// ===========================================

int64_t syscall_handler(uint64_t syscall_num, 
                        uint64_t arg1, 
                        uint64_t arg2, 
                        uint64_t arg3, 
                        uint64_t arg4, 
                        uint64_t arg5) {
    int64_t result = SYSCALL_ERROR;
    
    // Update statistics
    stats.total_syscalls++;
    
    // Validate syscall number
    if (syscall_num >= SYSCALL_MAX) {
        stats.invalid_syscalls++;
        return ENOSYS;  // Function not implemented
    }
    
    stats.syscall_counts[syscall_num]++;
    
    // Dispatch to appropriate handler
    switch (syscall_num) {
        // File I/O
        case SYS_READ:
            result = sys_read((int)arg1, (void*)arg2, arg3);
            break;
            
        case SYS_WRITE:
            result = sys_write((int)arg1, (const void*)arg2, arg3);
            break;
            
        case SYS_OPEN:
            result = sys_open((const char*)arg1, (int)arg2, (int)arg3);
            break;
            
        case SYS_CLOSE:
            result = sys_close((int)arg1);
            break;
            
        // Process management
        case SYS_EXIT:
            result = sys_exit((int)arg1);
            break;
            
        case SYS_GETPID:
            result = sys_getpid();
            break;
            
        case SYS_FORK:
            result = sys_fork();
            break;
            
        case SYS_EXECVE:
            result = sys_execve((const char*)arg1, (char* const*)arg2, (char* const*)arg3);
            break;
            
        // Memory management
        case SYS_BRK:
            result = sys_brk((void*)arg1);
            break;
            
        case SYS_MMAP:
            result = sys_mmap((void*)arg1, arg2, (int)arg3, (int)arg4, (int)arg5, 0);
            break;
            
        case SYS_MUNMAP:
            result = sys_munmap((void*)arg1, arg2);
            break;
            
        // AscentOS-specific
        case SYS_ASCENT_DEBUG:
            result = sys_ascent_debug((const char*)arg1);
            break;
            
        case SYS_ASCENT_INFO:
            result = sys_ascent_info((void*)arg1, arg2);
            break;
            
        case SYS_ASCENT_YIELD:
            result = sys_ascent_yield();
            break;
            
        case SYS_ASCENT_SLEEP:
            result = sys_ascent_sleep(arg1);
            break;
            
        case SYS_ASCENT_GETTIME:
            result = sys_ascent_gettime();
            break;
            
        default:
            stats.invalid_syscalls++;
            result = ENOSYS;  // Function not implemented
            break;
    }
    
    // Track failed syscalls
    if (result < 0) {
        stats.failed_syscalls++;
    }
    
    return result;
}

// ===========================================
// FILE I/O SYSCALLS
// ===========================================

int64_t sys_read(int fd, void* buf, uint64_t count) {
    // TODO: Implement file descriptor table and read operation
    // For now, return not implemented
    (void)fd;
    (void)buf;
    (void)count;
    return ENOSYS;
}

int64_t sys_write(int fd, const void* buf, uint64_t count) {
    // Special case: fd 1 (stdout) and fd 2 (stderr) -> serial output
    if (fd == 1 || fd == 2) {
        // Write to serial port
        const char* str = (const char*)buf;
        for (uint64_t i = 0; i < count; i++) {
            // Simple character output
            extern void serial_putchar(char c);
            serial_putchar(str[i]);
        }
        return (int64_t)count;
    }
    
    // TODO: Implement file descriptor table and write operation
    return ENOSYS;
}

int64_t sys_open(const char* path, int flags, int mode) {
    // TODO: Implement file opening
    (void)path;
    (void)flags;
    (void)mode;
    return ENOSYS;
}

int64_t sys_close(int fd) {
    // TODO: Implement file closing
    (void)fd;
    return ENOSYS;
}

// ===========================================
// PROCESS MANAGEMENT SYSCALLS
// ===========================================

int64_t sys_exit(int status) {
    serial_print("[SYSCALL] Task exiting with status: ");
    char buf[16];
    int_to_str(status, buf);
    serial_print(buf);
    serial_print("\n");
    
    // Terminate current task
    task_exit();
    
    // This should never return
    return 0;
}

int64_t sys_getpid(void) {
    task_t* current = task_get_current();
    if (!current) {
        return -1;
    }
    return current->pid;
}

int64_t sys_fork(void) {
    // TODO: Implement fork (process cloning)
    return ENOSYS;
}

int64_t sys_execve(const char* path, char* const argv[], char* const envp[]) {
    // TODO: Implement execve (load and execute program)
    (void)path;
    (void)argv;
    (void)envp;
    return ENOSYS;
}

// ===========================================
// MEMORY MANAGEMENT SYSCALLS
// ===========================================

int64_t sys_brk(void* addr) {
    // TODO: Implement heap management
    (void)addr;
    return ENOSYS;
}

int64_t sys_mmap(void* addr, uint64_t length, int prot, int flags, int fd, uint64_t offset) {
    // TODO: Implement memory mapping
    (void)addr;
    (void)length;
    (void)prot;
    (void)flags;
    (void)fd;
    (void)offset;
    return ENOSYS;
}

int64_t sys_munmap(void* addr, uint64_t length) {
    // TODO: Implement memory unmapping
    (void)addr;
    (void)length;
    return ENOSYS;
}

// ===========================================
// ASCENTOS-SPECIFIC SYSCALLS
// ===========================================

int64_t sys_ascent_debug(const char* message) {
    // Print debug message to serial
    serial_print("[USER DEBUG] ");
    serial_print(message);
    serial_print("\n");
    return SYSCALL_SUCCESS;
}

int64_t sys_ascent_info(void* info_buffer, uint64_t buffer_size) {
    // TODO: Fill info buffer with system information
    (void)info_buffer;
    (void)buffer_size;
    return ENOSYS;
}

int64_t sys_ascent_yield(void) {
    // Yield CPU to another task
    scheduler_yield();
    return SYSCALL_SUCCESS;
}

int64_t sys_ascent_sleep(uint64_t milliseconds) {
    // TODO: Implement proper sleep (block task for N milliseconds)
    // For now, just yield
    (void)milliseconds;
    scheduler_yield();
    return SYSCALL_SUCCESS;
}

int64_t sys_ascent_gettime(void) {
    // Return system uptime in ticks
    return (int64_t)get_system_ticks();
}

// ===========================================
// STATISTICS & DEBUGGING
// ===========================================

void syscall_get_stats(syscall_stats_t* out_stats) {
    if (!out_stats) return;
    
    out_stats->total_syscalls = stats.total_syscalls;
    out_stats->invalid_syscalls = stats.invalid_syscalls;
    out_stats->failed_syscalls = stats.failed_syscalls;
    
    for (int i = 0; i < SYSCALL_MAX; i++) {
        out_stats->syscall_counts[i] = stats.syscall_counts[i];
    }
}

void syscall_print_stats(void) {
    serial_print("\n=== Syscall Statistics ===\n");
    
    char num[32];
    
    serial_print("Total syscalls: ");
    uint64_to_string(stats.total_syscalls, num);
    serial_print(num);
    serial_print("\n");
    
    serial_print("Invalid syscalls: ");
    uint64_to_string(stats.invalid_syscalls, num);
    serial_print(num);
    serial_print("\n");
    
    serial_print("Failed syscalls: ");
    uint64_to_string(stats.failed_syscalls, num);
    serial_print(num);
    serial_print("\n");
    
    serial_print("\nTop syscalls:\n");
    
    // Find top 5 most used syscalls
    struct { int num; uint64_t count; } top[5] = {{0}};
    
    for (int i = 0; i < SYSCALL_MAX; i++) {
        if (stats.syscall_counts[i] > 0) {
            // Find insertion point
            for (int j = 0; j < 5; j++) {
                if (stats.syscall_counts[i] > top[j].count) {
                    // Shift down
                    for (int k = 4; k > j; k--) {
                        top[k] = top[k-1];
                    }
                    top[j].num = i;
                    top[j].count = stats.syscall_counts[i];
                    break;
                }
            }
        }
    }
    
    // Print top syscalls
    for (int i = 0; i < 5; i++) {
        if (top[i].count == 0) break;
        
        serial_print("  Syscall ");
        int_to_str(top[i].num, num);
        serial_print(num);
        serial_print(": ");
        uint64_to_string(top[i].count, num);
        serial_print(num);
        serial_print(" calls\n");
    }
    
    serial_print("\n");
}

void syscall_reset_stats(void) {
    stats.total_syscalls = 0;
    stats.invalid_syscalls = 0;
    stats.failed_syscalls = 0;
    
    for (int i = 0; i < SYSCALL_MAX; i++) {
        stats.syscall_counts[i] = 0;
    }
    
    serial_print("[SYSCALL] Statistics reset\n");
}