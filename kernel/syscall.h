// syscall.h - System Call Interface for AscentOS 64-bit
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

// ===========================================
// SYSCALL NUMBERS
// ===========================================
// Linux-compatible syscall numbers for easier porting

#define SYS_READ        0   // read(fd, buf, count)
#define SYS_WRITE       1   // write(fd, buf, count)
#define SYS_OPEN        2   // open(path, flags, mode)
#define SYS_CLOSE       3   // close(fd)
#define SYS_STAT        4   // stat(path, statbuf)
#define SYS_FSTAT       5   // fstat(fd, statbuf)
#define SYS_LSEEK       8   // lseek(fd, offset, whence)
#define SYS_MMAP        9   // mmap(addr, length, prot, flags, fd, offset)
#define SYS_MUNMAP      11  // munmap(addr, length)
#define SYS_BRK         12  // brk(addr)

#define SYS_EXIT        60  // exit(status)
#define SYS_GETPID      39  // getpid()
#define SYS_FORK        57  // fork()
#define SYS_EXECVE      59  // execve(path, argv, envp)
#define SYS_WAIT4       61  // wait4(pid, status, options, rusage)

#define SYS_KILL        62  // kill(pid, sig)
#define SYS_GETUID      102 // getuid()
#define SYS_GETGID      104 // getgid()

// AscentOS-specific syscalls (starting from 300)
#define SYS_ASCENT_DEBUG    300 // Debug print to serial
#define SYS_ASCENT_INFO     301 // Get system info
#define SYS_ASCENT_YIELD    302 // Yield CPU to another task
#define SYS_ASCENT_SLEEP    303 // Sleep for N milliseconds
#define SYS_ASCENT_GETTIME  304 // Get system uptime in ticks

// Maximum syscall number
#define SYSCALL_MAX     305

// ===========================================
// SYSCALL RETURN VALUES
// ===========================================

#define SYSCALL_SUCCESS     0
#define SYSCALL_ERROR      -1

// Error codes (negative values, POSIX-like)
#define ENOENT      -2   // No such file or directory
#define EBADF       -9   // Bad file descriptor
#define ENOMEM      -12  // Out of memory
#define EACCES      -13  // Permission denied
#define EINVAL      -22  // Invalid argument
#define ENOSYS      -38  // Function not implemented

// ===========================================
// SYSCALL INITIALIZATION
// ===========================================

// Initialize syscall system (setup MSRs, etc.)
void syscall_init(void);

// Check if syscalls are enabled
int syscall_is_enabled(void);

// ===========================================
// SYSCALL HANDLER (called from assembly)
// ===========================================

// Main syscall dispatcher - called from syscall64.asm
// Arguments arrive in registers: rdi, rsi, rdx, r10, r8, r9
// (r10 used instead of rcx because SYSCALL uses rcx for return address)
int64_t syscall_handler(uint64_t syscall_num, 
                        uint64_t arg1, 
                        uint64_t arg2, 
                        uint64_t arg3, 
                        uint64_t arg4, 
                        uint64_t arg5);

// ===========================================
// INDIVIDUAL SYSCALL IMPLEMENTATIONS
// ===========================================

// File I/O
int64_t sys_read(int fd, void* buf, uint64_t count);
int64_t sys_write(int fd, const void* buf, uint64_t count);
int64_t sys_open(const char* path, int flags, int mode);
int64_t sys_close(int fd);

// Process management
int64_t sys_exit(int status);
int64_t sys_getpid(void);
int64_t sys_fork(void);
int64_t sys_execve(const char* path, char* const argv[], char* const envp[]);

// Memory management
int64_t sys_brk(void* addr);
int64_t sys_mmap(void* addr, uint64_t length, int prot, int flags, int fd, uint64_t offset);
int64_t sys_munmap(void* addr, uint64_t length);

// AscentOS-specific
int64_t sys_ascent_debug(const char* message);
int64_t sys_ascent_info(void* info_buffer, uint64_t buffer_size);
int64_t sys_ascent_yield(void);
int64_t sys_ascent_sleep(uint64_t milliseconds);
int64_t sys_ascent_gettime(void);

// ===========================================
// USERSPACE SYSCALL WRAPPERS
// ===========================================
// These will be used by usermode programs
// (Usually in a separate userspace library)

#ifdef USERSPACE

static inline int64_t syscall0(uint64_t num) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t syscall1(uint64_t num, uint64_t arg1) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

// Userspace wrapper functions
#define write(fd, buf, count) syscall3(SYS_WRITE, fd, (uint64_t)(buf), count)
#define read(fd, buf, count) syscall3(SYS_READ, fd, (uint64_t)(buf), count)
#define exit(status) syscall1(SYS_EXIT, status)
#define getpid() syscall0(SYS_GETPID)
#define ascent_debug(msg) syscall1(SYS_ASCENT_DEBUG, (uint64_t)(msg))
#define ascent_yield() syscall0(SYS_ASCENT_YIELD)

#endif // USERSPACE

// ===========================================
// STATISTICS & DEBUGGING
// ===========================================

typedef struct {
    uint64_t total_syscalls;
    uint64_t syscall_counts[SYSCALL_MAX];
    uint64_t invalid_syscalls;
    uint64_t failed_syscalls;
} syscall_stats_t;

// Get syscall statistics
void syscall_get_stats(syscall_stats_t* stats);

// Print syscall statistics
void syscall_print_stats(void);

// Reset statistics
void syscall_reset_stats(void);

#endif // SYSCALL_H