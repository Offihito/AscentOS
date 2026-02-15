// syscall.h - System Call Interface for AscentOS 64-bit
// PHASE 3: Expanded Syscall Interface
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
#define SYS_PIPE        22  // pipe(pipefd[2])
#define SYS_DUP         32  // dup(oldfd)
#define SYS_DUP2        33  // dup2(oldfd, newfd)

#define SYS_EXIT        60  // exit(status)
#define SYS_GETPID      39  // getpid()
#define SYS_FORK        57  // fork()
#define SYS_EXECVE      59  // execve(path, argv, envp)
#define SYS_WAIT4       61  // wait4(pid, status, options, rusage)
#define SYS_WAITPID     7   // waitpid(pid, status, options)

#define SYS_KILL        62  // kill(pid, sig)
#define SYS_GETUID      102 // getuid()
#define SYS_GETGID      104 // getgid()

// AscentOS-specific syscalls (starting from 300)
#define SYS_ASCENT_DEBUG    300 // Debug print to serial
#define SYS_ASCENT_INFO     301 // Get system info
#define SYS_ASCENT_YIELD    302 // Yield CPU to another task
#define SYS_ASCENT_SLEEP    303 // Sleep for N milliseconds
#define SYS_ASCENT_GETTIME  304 // Get system uptime in ticks
#define SYS_ASCENT_SHMGET   305 // Shared memory: get/create segment
#define SYS_ASCENT_SHMMAP   306 // Shared memory: map into address space
#define SYS_ASCENT_SHMUNMAP 307 // Shared memory: unmap
#define SYS_ASCENT_MSGPOST  308 // Message queue: post message
#define SYS_ASCENT_MSGRECV  309 // Message queue: receive message

// Maximum syscall number
#define SYSCALL_MAX     310

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
#define EFAULT      -14  // Bad address
#define EBUSY       -16  // Resource busy
#define EEXIST      -17  // File exists
#define EINVAL      -22  // Invalid argument
#define EMFILE      -24  // Too many open files
#define ENOSPC      -28  // No space left on device
#define EPIPE       -32  // Broken pipe
#define ENOSYS      -38  // Function not implemented
#define ECHILD      -10  // No child processes
#define EAGAIN      -11  // Try again

// ===========================================
// FILE DESCRIPTOR CONSTANTS
// ===========================================

#define STDIN_FD    0
#define STDOUT_FD   1
#define STDERR_FD   2

#define MAX_OPEN_FILES   16  // Maximum open FDs per process
#define MAX_GLOBAL_FILES 32  // Maximum open files system-wide

// File descriptor types
#define FD_TYPE_NONE    0
#define FD_TYPE_SERIAL  1   // Serial/TTY
#define FD_TYPE_FAT32   2   // FAT32 file
#define FD_TYPE_PIPE_R  3   // Pipe read end
#define FD_TYPE_PIPE_W  4   // Pipe write end
#define FD_TYPE_KBUF    5   // Kernel circular buffer

// Open flags (O_*)
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400

// Seek whence values
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

// ===========================================
// FILE DESCRIPTOR TABLE
// ===========================================

// Per-process file descriptor entry
typedef struct {
    int       type;          // FD_TYPE_*
    int       flags;         // O_RDONLY / O_WRONLY / O_RDWR
    uint64_t  offset;        // Current position in file
    uint32_t  ref_count;     // Reference count (for dup)
    char      path[32];      // File path (for FAT32)
    void*     private_data;  // Type-specific pointer (pipe buf, etc.)
} fd_entry_t;

// ===========================================
// PIPE BUFFER
// ===========================================

#define PIPE_BUF_SIZE  256  // Internal pipe buffer size

typedef struct {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;
    int      write_end_open;  // 1 if write end still open
    int      read_end_open;   // 1 if read end still open
} pipe_buf_t;

// ===========================================
// STAT STRUCTURE
// ===========================================

typedef struct {
    uint32_t  st_mode;    // File type and permissions
    uint32_t  st_size;    // File size in bytes
    uint32_t  st_blocks;  // Number of 512B blocks
} ascent_stat_t;

// stat st_mode bits
#define S_IFREG   0x8000   // Regular file
#define S_IFCHR   0x2000   // Character device (serial)
#define S_IFIFO   0x1000   // FIFO / pipe
#define S_IRUSR   0x0100
#define S_IWUSR   0x0080
#define S_IRGRP   0x0020
#define S_IWGRP   0x0010
#define S_IROTH   0x0004
#define S_IWOTH   0x0002

// ===========================================
// SHARED MEMORY (IPC)
// ===========================================

#define SHM_MAX_SEGS   8
#define SHM_SEG_SIZE   4096   // 4KB per shared segment (one page)

typedef struct {
    int      id;
    int      in_use;
    uint8_t  data[SHM_SEG_SIZE];
    uint32_t owner_pid;
} shm_segment_t;

// ===========================================
// MESSAGE QUEUE (IPC)
// ===========================================

#define MSG_MAX_QUEUES   4
#define MSG_MAX_MSGS     8
#define MSG_MAX_SIZE     64

typedef struct {
    uint32_t sender_pid;
    uint32_t size;
    uint8_t  data[MSG_MAX_SIZE];
} ipc_message_t;

typedef struct {
    int          id;
    int          in_use;
    ipc_message_t msgs[MSG_MAX_MSGS];
    uint32_t     head;
    uint32_t     tail;
    uint32_t     count;
} msg_queue_t;

// ===========================================
// SYSCALL INITIALIZATION
// ===========================================

void syscall_init(void);
int  syscall_is_enabled(void);

// ===========================================
// SYSCALL HANDLER (called from assembly)
// ===========================================

int64_t syscall_handler(uint64_t syscall_num,
                        uint64_t arg1,
                        uint64_t arg2,
                        uint64_t arg3,
                        uint64_t arg4,
                        uint64_t arg5);

// ===========================================
// FILE DESCRIPTOR TABLE API
// ===========================================

// Allocate/free FD table for the current process
void     fd_table_init(fd_entry_t* table);
int      fd_alloc(fd_entry_t* table, int type, int flags, const char* path);
void     fd_free(fd_entry_t* table, int fd);
fd_entry_t* fd_get(fd_entry_t* table, int fd);

// Get fd table for the current task (implemented in syscall.c)
fd_entry_t* syscall_get_fd_table(void);

// ===========================================
// INDIVIDUAL SYSCALL IMPLEMENTATIONS
// ===========================================

// File I/O
int64_t sys_read(int fd, void* buf, uint64_t count);
int64_t sys_write(int fd, const void* buf, uint64_t count);
int64_t sys_open(const char* path, int flags, int mode);
int64_t sys_close(int fd);
int64_t sys_stat(const char* path, ascent_stat_t* st);
int64_t sys_fstat(int fd, ascent_stat_t* st);
int64_t sys_lseek(int fd, int64_t offset, int whence);
int64_t sys_pipe(int pipefd[2]);
int64_t sys_dup(int oldfd);
int64_t sys_dup2(int oldfd, int newfd);

// Process management
int64_t sys_exit(int status);
int64_t sys_getpid(void);
int64_t sys_fork(void);
int64_t sys_execve(const char* path, char* const argv[], char* const envp[]);
int64_t sys_waitpid(int pid, int* status, int options);
int64_t sys_wait4(int pid, int* status, int options, void* rusage);
int64_t sys_kill(int pid, int sig);
int64_t sys_getuid(void);
int64_t sys_getgid(void);

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
int64_t sys_ascent_shmget(int id, uint64_t size);
int64_t sys_ascent_shmmap(int id);
int64_t sys_ascent_shmunmap(int id);
int64_t sys_ascent_msgpost(int queue_id, const void* data, uint64_t size);
int64_t sys_ascent_msgrecv(int queue_id, void* data, uint64_t max_size);

// ===========================================
// USERSPACE SYSCALL WRAPPERS
// ===========================================

#ifdef USERSPACE

static inline int64_t syscall0(uint64_t num) {
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall1(uint64_t num, uint64_t arg1) {
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num), "D"(arg1) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall2(uint64_t num, uint64_t arg1, uint64_t arg2) {
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num), "D"(arg1), "S"(arg2) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall3(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    int64_t ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3) : "rcx", "r11", "memory");
    return ret;
}

static inline int64_t syscall6(uint64_t num,
                                uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6) {
    int64_t ret;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8  __asm__("r8")  = a5;
    register uint64_t r9  __asm__("r9")  = a6;
    __asm__ volatile ("syscall"
        : "=a"(ret)
        : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return ret;
}

// Userspace wrapper macros
#define write(fd, buf, count)       syscall3(SYS_WRITE, (uint64_t)(fd), (uint64_t)(buf), (uint64_t)(count))
#define read(fd, buf, count)        syscall3(SYS_READ,  (uint64_t)(fd), (uint64_t)(buf), (uint64_t)(count))
#define open(path, flags, mode)     syscall3(SYS_OPEN,  (uint64_t)(path), (uint64_t)(flags), (uint64_t)(mode))
#define close(fd)                   syscall1(SYS_CLOSE, (uint64_t)(fd))
#define lseek(fd, off, whence)      syscall3(SYS_LSEEK, (uint64_t)(fd), (uint64_t)(off), (uint64_t)(whence))
#define dup(oldfd)                  syscall1(SYS_DUP,   (uint64_t)(oldfd))
#define dup2(oldfd, newfd)          syscall2(SYS_DUP2,  (uint64_t)(oldfd), (uint64_t)(newfd))
#define exit(status)                syscall1(SYS_EXIT,  (uint64_t)(status))
#define getpid()                    syscall0(SYS_GETPID)
#define waitpid(pid, st, opts)      syscall3(SYS_WAITPID,(uint64_t)(pid),(uint64_t)(st),(uint64_t)(opts))
#define kill(pid, sig)              syscall2(SYS_KILL,  (uint64_t)(pid), (uint64_t)(sig))
#define ascent_debug(msg)           syscall1(SYS_ASCENT_DEBUG,   (uint64_t)(msg))
#define ascent_yield()              syscall0(SYS_ASCENT_YIELD)
#define ascent_sleep(ms)            syscall1(SYS_ASCENT_SLEEP,   (uint64_t)(ms))
#define ascent_gettime()            syscall0(SYS_ASCENT_GETTIME)
#define ascent_shmget(id, sz)       syscall2(SYS_ASCENT_SHMGET,  (uint64_t)(id), (uint64_t)(sz))
#define ascent_shmmap(id)           syscall1(SYS_ASCENT_SHMMAP,  (uint64_t)(id))
#define ascent_shmunmap(id)         syscall1(SYS_ASCENT_SHMUNMAP,(uint64_t)(id))
#define ascent_msgpost(q,d,sz)      syscall3(SYS_ASCENT_MSGPOST, (uint64_t)(q),(uint64_t)(d),(uint64_t)(sz))
#define ascent_msgrecv(q,d,sz)      syscall3(SYS_ASCENT_MSGRECV, (uint64_t)(q),(uint64_t)(d),(uint64_t)(sz))

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

void syscall_get_stats(syscall_stats_t* stats);
void syscall_print_stats(void);
void syscall_reset_stats(void);

#endif // SYSCALL_H