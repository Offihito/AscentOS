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
#define SYS_BRK 12
#define SYS_RT_SIGACTION 13
#define SYS_IOCTL 16
#define SYS_WRITEV 20
#define SYS_NANOSLEEP 35
#define SYS_FTRUNCATE 77
#define SYS_RT_SIGPROCMASK 14
#define SYS_SIGALTSTACK 131
#define SYS_GETPID 39
#define SYS_FORK 57
#define SYS_EXECVE 59
#define SYS_EXIT 60
#define SYS_WAIT4 61
#define SYS_FCNTL 72
#define SYS_ARCH_PRCTL 158
#define SYS_SIGPROCMASK 186
#define SYS_TGKILL 200
#define SYS_CLOCK_GETTIME 228
#define SYS_GETDENTS64 217
#define SYS_SET_TID_ADDRESS 218
#define SYS_EXIT_GROUP 231
#define SYS_GETRANDOM 318

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
void syscall_register_media(void);

// ── Core init (MSRs + calls subsystem registrations) ────────────────────────
void syscall_init(void);

#endif
