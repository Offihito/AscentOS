#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <sched.h>
#include <errno.h>
#include <stdatomic.h>

// Syscall numbers for x86_64
#ifndef __NR_clone
#define __NR_clone 56
#endif
#ifndef __NR_futex
#define __NR_futex 202
#endif

// Direct syscall wrappers
static long raw_clone(unsigned long flags, void *child_stack, int *ptid, int *ctid, unsigned long newtls) {
    long ret;
    // On x86_64, clone is: 
    // %rax = 56, %rdi = flags, %rsi = child_stack, %rdx = ptid, %r10 = ctid, %r8 = newtls
    __asm__ volatile(
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "movq %5, %%r10\n"
        "movq %6, %%r8\n"
        "movq $56, %%rax\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r" (ret)
        : "r" (flags), "r" (flags), "r" (child_stack), "r" (ptid), "r" (ctid), "r" (newtls)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "rcx", "r11", "memory"
    );
    return ret;
}

static long raw_futex(uint32_t *uaddr, int op, uint32_t val, const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3) {
    long ret;
    __asm__ volatile(
        "movq %1, %%rdi\n"
        "movq %2, %%rsi\n"
        "movq %3, %%rdx\n"
        "movq %4, %%r10\n"
        "movq %5, %%r8\n"
        "movq %6, %%r9\n"
        "movq $202, %%rax\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r" (ret)
        : "r" (uaddr), "r" ((long)op), "r" ((long)val), "r" (timeout), "r" (uaddr2), "r" ((long)val3)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "r9", "rcx", "r11", "memory"
    );
    return ret;
}

#define STACK_SIZE (1024 * 1024)
static uint8_t child_stack[STACK_SIZE] __attribute__((aligned(16)));

static _Atomic uint32_t futex_word = 0;
static _Atomic int child_done = 0;

int thread_main(void *arg) {
    (void)arg;
    printf("[CHILD] Thread started, waiting for futex...\n");
    
    // Wait for futex_word to become 1
    while (atomic_load(&futex_word) == 0) {
        raw_futex((uint32_t *)&futex_word, FUTEX_WAIT, 0, NULL, NULL, 0);
    }
    
    printf("[CHILD] Woke up! futex_word = %u\n", atomic_load(&futex_word));
    atomic_store(&child_done, 1);
    return 0;
}

int main() {
    printf("=== AscentOS Clone & Futex Test ===\n");

    // Test 1: Simple futex wait/wake in same process
    printf("\n--- Test 1: Single-process Futex ---\n");
    futex_word = 0;
    struct timespec ts = {0, 100000000}; // 100ms
    long ret = raw_futex((uint32_t *)&futex_word, FUTEX_WAIT, 0, &ts, NULL, 0);
    if (ret == -1 && errno == ETIMEDOUT) {
        printf("[PASS] Futex timeout worked\n");
    } else {
        printf("[INFO] Futex wait returned %ld (errno %d)\n", ret, errno);
    }

    // Test 2: Clone for thread creation
    printf("\n--- Test 2: Threaded Clone & Futex ---\n");
    futex_word = 0;
    child_done = 0;

    unsigned long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM;
    
    // On x86_64, the stack grows down, and the child_stack points to the TOP of the allocation.
    void *stack_top = child_stack + STACK_SIZE;

    printf("[PARENT] Spawning child thread...\n");
    long pid = raw_clone(flags, stack_top, NULL, NULL, 0);

    if (pid < 0) {
        perror("clone");
        return 1;
    }

    if (pid == 0) {
        // Child thread
        // We can't easily call printf here if the stack/TLS isn't perfectly set up by clone()
        // but for a 1:1 linux clone it should work if musl handles it.
        // However, raw_clone doesn't set up musl's thread structure.
        // So we'll just do a raw syscall for exit to be safe.
        thread_main(NULL);
        syscall(SYS_exit, 0);
    }

    // Parent thread
    printf("[PARENT] Child PID: %ld\n", pid);
    
    printf("[PARENT] Sleeping for a bit...\n");
    usleep(200000); // 200ms

    printf("[PARENT] Setting futex_word = 1 and waking child...\n");
    atomic_store(&futex_word, 1);
    ret = raw_futex((uint32_t *)&futex_word, FUTEX_WAKE, 1, NULL, NULL, 0);
    printf("[PARENT] Woke %ld threads\n", ret);

    int timeout = 100;
    while (!atomic_load(&child_done) && timeout-- > 0) {
        usleep(10000);
    }

    if (atomic_load(&child_done)) {
        printf("[PASS] Child thread finished successfully\n");
    } else {
        printf("[FAIL] Child thread did not finish\n");
    }

    return 0;
}
