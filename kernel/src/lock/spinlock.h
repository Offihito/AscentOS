#ifndef LOCK_SPINLOCK_H
#define LOCK_SPINLOCK_H

#include <stdbool.h>

typedef struct {
    bool locked;
} spinlock_t;

#define SPINLOCK_INIT {false}

static inline void spinlock_init(spinlock_t *lock) {
    lock->locked = false;
}

static inline void spinlock_acquire(spinlock_t *lock) {
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause" ::: "memory");
    }
}

static inline void spinlock_release(spinlock_t *lock) {
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}

#endif
