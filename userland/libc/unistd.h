#ifndef LIBC_UNISTD_H
#define LIBC_UNISTD_H

// ─────────────────────────────────────────────
//  AscentOS Minimal Libc — unistd.h
//  Temel POSIX-benzeri syscall wrapper'ları
//  (read, write, exit, getpid, sleep, fork,
//   waitpid, pipe)
// ─────────────────────────────────────────────

#include "types.h"
#include "syscall.h"

static inline ssize_t write(int fd, const void* buf, size_t len) {
    return syscall3(SYS_WRITE, fd, (long)buf, (long)len);
}

static inline ssize_t read(int fd, void* buf, size_t len) {
    return syscall3(SYS_READ, fd, (long)buf, (long)len);
}

static inline void exit(int code) {
    syscall1(SYS_EXIT, code);
    __builtin_unreachable();
}

static inline pid_t getpid(void) {
    return (pid_t)syscall1(SYS_GETPID, 0);
}

static inline void sleep(int ticks) {
    syscall1(SYS_SLEEP, ticks);
}

static inline void yield(void) {
    syscall1(5, 0);  // SYS_YIELD = 5
}

static inline pid_t fork(void) {
    long ret;
    __asm__ volatile (
        "mov $19, %%rax\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(ret)
        :
        : "rax", "rcx", "r11", "memory"
    );
    return (pid_t)ret;
}

static inline pid_t waitpid(pid_t pid, int* status, int opts) {
    return (pid_t)syscall3(SYS_WAITPID, pid, (long)status, opts);
}

static inline int pipe(int fds[2]) {
    return (int)syscall1(SYS_PIPE, (long)fds);
}

#endif // LIBC_UNISTD_H