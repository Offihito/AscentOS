#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

// ── MSR Constants ───────────────────────────────────────────────────────────
#define IA32_EFER 0xC0000080
#define IA32_STAR 0xC0000081
#define IA32_LSTAR 0xC0000082
#define IA32_FMASK 0xC0000084

#define IA32_EFER_SCE 0x01

// ── Syscall Numbers (Linux x86_64 ABI) ──────────────────────────────────────
#define SYS_READ 0
#define SYS_WRITE 1
#define SYS_OPEN 2
#define SYS_CLOSE 3
#define SYS_STAT 4
#define SYS_FSTAT 5
#define SYS_POLL 7
#define SYS_LSEEK 8
#define SYS_MMAP 9
#define SYS_MPROTECT 10
#define SYS_MUNMAP 11
#define SYS_MREMAP 25
#define SYS_BRK 12
#define SYS_RT_SIGACTION 13
#define SYS_RT_SIGPROCMASK 14
#define SYS_RT_SIGRETURN 15
#define SYS_IOCTL 16
#define SYS_READV 19
#define SYS_WRITEV 20
#define SYS_ACCESS 21
#define SYS_PIPE 22
#define SYS_DUP 32
#define SYS_DUP2 33
#define SYS_NANOSLEEP 35
#define SYS_SOCKET 41
#define SYS_CONNECT 42
#define SYS_ACCEPT 43
#define SYS_SENDTO 44
#define SYS_RECVFROM 45
#define SYS_BIND 49
#define SYS_LISTEN 50
#define SYS_FCNTL 72
#define SYS_FTRUNCATE 77
#define SYS_GETCWD 79
#define SYS_CHDIR 80
#define SYS_MKDIR 83
#define SYS_RMDIR 84
#define SYS_RENAME 82
#define SYS_UNLINK 87
#define SYS_SYMLINK 88
#define SYS_READLINK 89
#define SYS_CHMOD 90
#define SYS_CHOWN 92
#define SYS_UMASK 95
#define SYS_GETUID 102
#define SYS_GETGID 104
#define SYS_GETEUID 107
#define SYS_GETEGID 108
#define SYS_GETRESUID 118
#define SYS_GETRESGID 120
#define SYS_RT_SIGPROCMASK 14
#define SYS_SIGALTSTACK 131
#define SYS_GETPID 39
#define SYS_FORK 57
#define SYS_CLONE 56
#define SYS_EXECVE 59
#define SYS_EXIT 60
#define SYS_WAIT4 61
#define SYS_KILL 62
#define SYS_UNAME 63
#define SYS_FCNTL 72
#define SYS_SETPGID 109
#define SYS_GETPPID 110
#define SYS_GETPGRP 111
#define SYS_SETSID 112
#define SYS_ARCH_PRCTL 158
#define SYS_PRCTL 157
#define SYS_SIGPROCMASK 186
#define SYS_TGKILL 200
#define SYS_CLOCK_GETTIME 228
#define SYS_GETDENTS64 217
#define SYS_SET_TID_ADDRESS 218
#define SYS_EXIT_GROUP 231
#define SYS_PIPE2 293
#define SYS_GETRANDOM 318
#define SYS_OPENAT 257
#define SYS_NEWFSTATAT 262
#define SYS_FACCESSAT2 439

#define MAX_SYSCALL 512

// ── Register state pushed by syscall_entry.asm ──────────────────────────────
struct syscall_regs {
  uint64_t rdi, rsi, rdx, r10, r8, r9, rax, rbx, rbp, r12, r13, r14, r15;
  uint64_t rip, rflags, rsp;
} __attribute__((packed));

// ── Syscall handler types ───────────────────────────────────────────────────
// Standard handler: receives the 6 argument registers, returns result in rax.
typedef uint64_t (*syscall_handler_t)(uint64_t, uint64_t, uint64_t, uint64_t,
                                      uint64_t, uint64_t);

// Raw handler: receives the full saved register frame.  Used by syscalls
// that need access to the caller's RIP/RSP/RFLAGS (e.g. fork).
typedef uint64_t (*syscall_raw_handler_t)(struct syscall_regs *regs);

// ── Register a single syscall handler ───────────────────────────────────────
void syscall_register(int num, syscall_handler_t handler);

// ── Register a raw syscall handler (receives full register frame) ───────────
void syscall_register_raw(int num, syscall_raw_handler_t handler);

// ── Subsystem registration (called from syscall_init) ───────────────────────
void syscall_register_io(void);
void syscall_register_process(void);
void syscall_register_mm(void);
void syscall_register_arch(void);
void syscall_register_signal(void);
void signal_deliver_syscall(struct syscall_regs *regs);
void syscall_register_media(void);
void syscall_register_net(void);

// ── Core init (MSRs + calls subsystem registrations) ────────────────────────
void syscall_init(void);

// Allocate virtual address range from mmap region (for device mmap handlers)
uint64_t mm_alloc_mmap_region(uint64_t length);

#endif
