#ifndef SPINLOCK64_H
#define SPINLOCK64_H

// spinlock64.h — AscentOS 64-bit Spinlock
//
// KULLANIM:
//   spinlock_t lock = SPINLOCK_INIT;
//
//   spinlock_lock(&lock);
//   // ... kritik bölge ...
//   spinlock_unlock(&lock);
//
// INTERRUPT-SAFE kullanım (IRQ handler ile paylaşılan veri için):
//   uint64_t flags = spinlock_lock_irq(&lock);
//   // ... kritik bölge ...
//   spinlock_unlock_irq(&lock, flags);
//
// SMP NOTU:
//   Şu an tek CPU çalışıyor. lock/unlock atomik olarak işaretlenmiş
//   ve cpu_relax() ile doğru şekilde implemente edilmiştir.
//   AP'ler (Application Processors) eklendikçe doğrudan çalışacak.

#include <stdint.h>
#include "cpu64.h"

// ── Spinlock türü ─────────────────────────────────────────────────────────────
// 0 = serbest, 1 = kilitli
typedef struct {
    volatile uint32_t locked;
#ifdef SPINLOCK_DEBUG
    const char* owner_file;   // hangi dosyadan kilitlendi
    int         owner_line;   // hangi satırdan kilitlendi
#endif
} spinlock_t;

#define SPINLOCK_INIT   { .locked = 0 }

// ── Temel lock/unlock ─────────────────────────────────────────────────────────

// spinlock_lock — kilit alana kadar döner (busy-wait)
// xchg: atomik exchange — belleği oku ve yeni değer yaz tek işlemde
static inline void spinlock_lock(spinlock_t* lock) {
    while (1) {
        // xchgl: 1 yazıp eski değeri al — eski değer 0 ise kilidi aldık
        uint32_t old;
        __asm__ volatile (
            "xchgl %0, %1"
            : "=r"(old), "+m"(lock->locked)
            : "0"(1)
            : "memory"
        );
        if (old == 0) return;   // kilidi aldık

        // Kilitliydi — diğer CPU serbest bırakana kadar bekle
        // İç döngüde cpu_relax() ile pipeline'ı rahatlatıyoruz
        while (lock->locked)
            cpu_relax();
    }
}

// spinlock_trylock — kilidi almaya çalış, başaramazsan 0 döndür
static inline int spinlock_trylock(spinlock_t* lock) {
    uint32_t old;
    __asm__ volatile (
        "xchgl %0, %1"
        : "=r"(old), "+m"(lock->locked)
        : "0"(1)
        : "memory"
    );
    return (old == 0);   // 1 = başarılı, 0 = başarısız
}

// spinlock_unlock — kilidi serbest bırak
static inline void spinlock_unlock(spinlock_t* lock) {
    __asm__ volatile (
        "movl $0, %0"
        : "=m"(lock->locked)
        :
        : "memory"
    );
}

// spinlock_is_locked — kilidin durumunu sorgula (debug için)
static inline int spinlock_is_locked(const spinlock_t* lock) {
    return lock->locked != 0;
}

// ── Interrupt-safe lock/unlock ────────────────────────────────────────────────
// IRQ handler ile paylaşılan veri yapıları için kullanılır.
// Kilit alınırken interrupt'ları kapatır, serbest bırakırken eski duruma döner.

static inline uint64_t spinlock_lock_irq(spinlock_t* lock) {
    uint64_t flags = cpu_save_flags();   // interrupt'ları kapat + RFLAGS kaydet
    spinlock_lock(lock);
    return flags;
}

static inline void spinlock_unlock_irq(spinlock_t* lock, uint64_t flags) {
    spinlock_unlock(lock);
    cpu_restore_flags(flags);            // interrupt durumunu geri yükle
}

// ── Read-Write Spinlock ───────────────────────────────────────────────────────
// Basit spinlock tabanlı implementasyon — cmpxchg yerine xchg kullanır.
// Birden fazla okuyucu aynı anda çalışabilir.
// Yazıcı çalışırken hiçbir okuyucu/yazıcı giremez.
//
// Kullanım:
//   rwlock_t rw = RWLOCK_INIT;
//   rwlock_read_lock(&rw);    // okuma başlat
//   rwlock_read_unlock(&rw);  // okuma bitir
//   rwlock_write_lock(&rw);   // yazma başlat
//   rwlock_write_unlock(&rw); // yazma bitir

typedef struct {
    spinlock_t  write_lock;     // yazıcı kilidi
    volatile uint32_t readers;  // aktif okuyucu sayısı
} rwlock_t;

#define RWLOCK_INIT  { .write_lock = SPINLOCK_INIT, .readers = 0 }

static inline void rwlock_read_lock(rwlock_t* rw) {
    // Yazıcı varsa bekle
    while (spinlock_is_locked(&rw->write_lock))
        cpu_relax();
    // Okuyucu sayısını atomik artır
    __asm__ volatile ("lock incl %0" : "+m"(rw->readers) :: "memory", "cc");
    // Artırma sonrası yazıcı girmiş olabilir — tekrar kontrol et
    while (spinlock_is_locked(&rw->write_lock))
        cpu_relax();
}

static inline void rwlock_read_unlock(rwlock_t* rw) {
    __asm__ volatile ("lock decl %0" : "+m"(rw->readers) :: "memory", "cc");
}

static inline void rwlock_write_lock(rwlock_t* rw) {
    // Önce write_lock'u al
    spinlock_lock(&rw->write_lock);
    // Tüm okuyucular bitene kadar bekle
    while (rw->readers > 0)
        cpu_relax();
}

static inline void rwlock_write_unlock(rwlock_t* rw) {
    spinlock_unlock(&rw->write_lock);
}

#endif // SPINLOCK64_H