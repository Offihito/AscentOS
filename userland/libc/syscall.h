#ifndef LIBC_SYSCALL_H
#define LIBC_SYSCALL_H

// ─────────────────────────────────────────────
//  AscentOS Minimal Libc — syscall.h
//  Syscall numaraları ve raw syscall inline asm
// ─────────────────────────────────────────────

// ── Syscall Numaraları ────────────────────────
#define SYS_WRITE    1
#define SYS_READ     2
#define SYS_EXIT     3
#define SYS_GETPID   4
#define SYS_SLEEP    6
#define SYS_FORK     19
#define SYS_WAITPID  21
#define SYS_PIPE     22

// ── 1 Argüman ────────────────────────────────
static inline long syscall1(long nr, long a1) {
    long ret;
    __asm__ volatile (
        "mov %1, %%rax\n"
        "mov %2, %%rdi\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(ret)
        : "r"(nr), "r"(a1)
        : "rax", "rdi", "rcx", "r11", "memory"
    );
    return ret;
}

// ── 3 Argüman ────────────────────────────────
static inline long syscall3(long nr, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile (
        "mov %1, %%rax\n"
        "mov %2, %%rdi\n"
        "mov %3, %%rsi\n"
        "mov %4, %%rdx\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(ret)
        : "r"(nr), "r"(a1), "r"(a2), "r"(a3)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

#endif // LIBC_SYSCALL_H