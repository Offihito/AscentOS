#ifndef SPINLOCK64_H
#define SPINLOCK64_H
#include <stdint.h>
#include "cpu64.h"

// Spinlock type
typedef struct {
    volatile uint32_t locked;
#ifdef SPINLOCK_DEBUG
    const char* owner_file;   
    int         owner_line;  
#endif
} spinlock_t;

#define SPINLOCK_INIT   { .locked = 0 }

static inline void spinlock_lock(spinlock_t* lock) {
    while (1) {
        uint32_t old;
        __asm__ volatile (
            "xchgl %0, %1"
            : "=r"(old), "+m"(lock->locked)
            : "0"(1)
            : "memory"
        );
        if (old == 0) return;  

        while (lock->locked)
            cpu_relax();
    }
}

// spinlock_trylock try to get the lock without blocking. Returns 1 if successful, 0 if already locked.
static inline int spinlock_trylock(spinlock_t* lock) {
    uint32_t old;
    __asm__ volatile (
        "xchgl %0, %1"
        : "=r"(old), "+m"(lock->locked)
        : "0"(1)
        : "memory"
    );
    return (old == 0);   
}

static inline void spinlock_unlock(spinlock_t* lock) {
    __asm__ volatile (
        "movl $0, %0"
        : "=m"(lock->locked)
        :
        : "memory"
    );
}

static inline int spinlock_is_locked(const spinlock_t* lock) {
    return lock->locked != 0;
}

static inline uint64_t spinlock_lock_irq(spinlock_t* lock) {
    uint64_t flags = cpu_save_flags();  
    spinlock_lock(lock);
    return flags;
}

static inline void spinlock_unlock_irq(spinlock_t* lock, uint64_t flags) {
    spinlock_unlock(lock);
    cpu_restore_flags(flags);          
}

typedef struct {
    spinlock_t  write_lock;    
    volatile uint32_t readers; 
} rwlock_t;

#define RWLOCK_INIT  { .write_lock = SPINLOCK_INIT, .readers = 0 }

static inline void rwlock_read_lock(rwlock_t* rw) {
    while (spinlock_is_locked(&rw->write_lock))
        cpu_relax();
    __asm__ volatile ("lock incl %0" : "+m"(rw->readers) :: "memory", "cc");
    while (spinlock_is_locked(&rw->write_lock))
        cpu_relax();
}

static inline void rwlock_read_unlock(rwlock_t* rw) {
    __asm__ volatile ("lock decl %0" : "+m"(rw->readers) :: "memory", "cc");
}

static inline void rwlock_write_lock(rwlock_t* rw) {
    spinlock_lock(&rw->write_lock);
    while (rw->readers > 0)
        cpu_relax();
}

static inline void rwlock_write_unlock(rwlock_t* rw) {
    spinlock_unlock(&rw->write_lock);
}

#endif 