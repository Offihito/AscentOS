#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

// ── MSR Constants ───────────────────────────────────────────────────────────
#define IA32_EFER  0xC0000080
#define IA32_STAR  0xC0000081
#define IA32_LSTAR 0xC0000082
#define IA32_FMASK 0xC0000084

#define IA32_EFER_SCE 0x01

// ── Syscall Numbers (Linux x86_64 ABI) ──────────────────────────────────────
#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_CLOSE   3
#define SYS_EXIT   60

#define MAX_SYSCALL 61

// ── Syscall handler type ────────────────────────────────────────────────────
typedef uint64_t (*syscall_handler_t)(uint64_t, uint64_t, uint64_t,
                                      uint64_t, uint64_t, uint64_t);

// ── Register a single syscall handler ───────────────────────────────────────
void syscall_register(int num, syscall_handler_t handler);

// ── Subsystem registration (called from syscall_init) ───────────────────────
void syscall_register_io(void);
void syscall_register_process(void);

// ── Core init (MSRs + calls subsystem registrations) ────────────────────────
void syscall_init(void);

#endif
