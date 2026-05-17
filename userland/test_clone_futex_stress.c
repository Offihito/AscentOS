#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <sched.h>
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

// ── Simple Mutex Implementation ─────────────────────────────────────────────
typedef struct {
    _Atomic uint32_t val; // 0 = unlocked, 1 = locked, 2 = locked with waiters
} mutex_t;

void mutex_lock(mutex_t *m) {
    uint32_t expected = 0;
    // Fast path: try to grab the lock
    if (atomic_compare_exchange_strong(&m->val, &expected, 1)) {
        return;
    }
    
    // Slow path
    if (expected != 2) {
        expected = atomic_exchange(&m->val, 2);
    }
    while (expected != 0) {
        raw_futex((uint32_t *)&m->val, FUTEX_WAIT, 2, NULL, NULL, 0);
        expected = atomic_exchange(&m->val, 2);
    }
}

void mutex_unlock(mutex_t *m) {
    if (atomic_fetch_sub(&m->val, 1) != 1) {
        atomic_store(&m->val, 0);
        raw_futex((uint32_t *)&m->val, FUTEX_WAKE, 1, NULL, NULL, 0);
    }
}

// ── Stress Test ─────────────────────────────────────────────────────────────
#define NUM_THREADS 12
#define ITERATIONS 2000
#define STACK_SIZE (1024 * 64)

static uint8_t stacks[NUM_THREADS][STACK_SIZE] __attribute__((aligned(16)));
static mutex_t locks[4] = {{0}, {0}, {0}, {0}};
static volatile int shared_counters[4] = {0, 0, 0, 0};
static _Atomic int threads_done = 0;

int thread_worker(void *arg) {
    int id = (int)(long)arg;
    int lock_idx = id % 4;
    printf("[THREAD %d] Started (tracking lock %d)\n", id, lock_idx);
    
    for (int i = 0; i < ITERATIONS; i++) {
        // Lock our primary mutex
        mutex_lock(&locks[lock_idx]);
        shared_counters[lock_idx]++;
        
        // Occasionally try to grab another lock to test multi-wait/wake
        if (i % 7 == 0) {
            int other_idx = (lock_idx + 1) % 4;
            mutex_lock(&locks[other_idx]);
            shared_counters[other_idx]++;
            mutex_unlock(&locks[other_idx]);
        }
        
        mutex_unlock(&locks[lock_idx]);
        
        // Force context switch
        if (i % 13 == 0) {
            sched_yield();
        }
    }
    
    printf("[THREAD %d] Done\n", id);
    atomic_fetch_add(&threads_done, 1);
    
    syscall(SYS_exit, 0);
    return 0;
}

int main() {
    printf("=== AscentOS ULTRA Clone & Futex Stress Test ===\n");
    printf("Threads: %d, Iterations per thread: %d\n", NUM_THREADS, ITERATIONS);
    
    int expected_total = 0;
    // Calculate expected total sum across all counters
    // Each thread does ITERATIONS primary increments.
    // Each thread does floor(ITERATIONS/7) secondary increments.
    int secondary_per_thread = ITERATIONS / 7;
    expected_total = NUM_THREADS * (ITERATIONS + secondary_per_thread);
    printf("Expected sum of all counters: %d\n", expected_total);

    unsigned long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM;

    for (int i = 0; i < NUM_THREADS; i++) {
        void *stack_top = stacks[i] + STACK_SIZE;
        long tid = raw_clone(flags, stack_top, NULL, NULL, 0);
        
        if (tid < 0) {
            perror("clone");
            return 1;
        }
        if (tid == 0) {
            thread_worker((void *)(long)i);
        }
    }

    printf("[MAIN] All threads spawned, waiting for completion...\n");
    
    while (atomic_load(&threads_done) < NUM_THREADS) {
        usleep(50000);
    }

    printf("[MAIN] All threads finished!\n");
    
    int actual_total = 0;
    for (int i = 0; i < 4; i++) {
        printf("[MAIN] Counter %d: %d\n", i, shared_counters[i]);
        actual_total += shared_counters[i];
    }
    
    printf("[MAIN] Total sum: %d (Expected: %d)\n", actual_total, expected_total);
    
    if (actual_total == expected_total) {
        printf("[PASS] ULTRA STRESS TEST PASSED!\n");
    } else {
        printf("[FAIL] Counter mismatch detected!\n");
    }

    return 0;
}
