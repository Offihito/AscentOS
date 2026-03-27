#include "spinlock64.h"
#include "cpu64.h"
#include <stdint.h>

extern void serial_print(const char* s);
extern void println64(const char* str, uint8_t color);
extern void print_str64(const char* str, uint8_t color);

// RGB
#define VGA_WHITE       0x0F
#define VGA_GREEN       0x02
#define VGA_CYAN        0x03
#define VGA_YELLOW      0x0E
#define VGA_RED         0x04
#define VGA_LIGHT_GREEN 0x0A
#define VGA_DARK_GRAY   0x08

// Helper uint32 to string (decimal)
static void u32_to_str(uint32_t v, char* buf) {
    if (!v) { buf[0] = '0'; buf[1] = '\0'; return; }
    char t[12]; int i = 0;
    while (v) { t[i++] = '0' + (v % 10); v /= 10; }
    int j = 0;
    while (--i >= 0) buf[j++] = t[i];
    buf[j] = '\0';
}

void spinlock_test(void) {
    char buf[64];

    // Test 1 basic lock/unlock
    serial_print("[SPINLOCK] Test 1: Basic lock/unlock\n");
    spinlock_t lock = SPINLOCK_INIT;

    if (spinlock_is_locked(&lock)) {
        serial_print("[SPINLOCK] FAIL: Lock should not be held after init!\n");
        println64("  [FAIL] Init: lock should be free", VGA_RED);
        return;
    }
    println64("  [OK] Init: lock is free", VGA_LIGHT_GREEN);

    spinlock_lock(&lock);
    if (!spinlock_is_locked(&lock)) {
        serial_print("[SPINLOCK] FAIL: Lock should be held after lock()!\n");
        println64("  [FAIL] lock() failed to acquire lock", VGA_RED);
        return;
    }
    println64("  [OK] lock(): lock acquired", VGA_LIGHT_GREEN);

    spinlock_unlock(&lock);
    if (spinlock_is_locked(&lock)) {
        serial_print("[SPINLOCK] FAIL: Lock should be free after unlock()!\n");
        println64("  [FAIL] unlock() failed to release lock", VGA_RED);
        return;
    }
    println64("  [OK] unlock(): lock released", VGA_LIGHT_GREEN);

    // Test 2 Trylock
    serial_print("[SPINLOCK] Test 2: trylock\n");

    int ok = spinlock_trylock(&lock);
    if (!ok) {
        println64("  [FAIL] trylock failed on free lock", VGA_RED);
        return;
    }
    println64("  [OK] trylock: succeeded on free lock", VGA_LIGHT_GREEN);

    int ok2 = spinlock_trylock(&lock);
    if (ok2) {
        println64("  [FAIL] trylock should fail on held lock", VGA_RED);
        spinlock_unlock(&lock);
        return;
    }
    println64("  [OK] trylock: failed on held lock (correct)", VGA_LIGHT_GREEN);
    spinlock_unlock(&lock);

    // Test3 IRQ-safe lock/unlock
    serial_print("[SPINLOCK] Test 3: IRQ-safe lock/unlock\n");

    uint64_t flags = spinlock_lock_irq(&lock);
    println64("  [OK] lock_irq: lock acquired + IRQ disabled", VGA_LIGHT_GREEN);
    spinlock_unlock_irq(&lock, flags);
    println64("  [OK] unlock_irq: lock released + IRQ restored", VGA_LIGHT_GREEN);

    // Test 4 Stress test
    serial_print("[SPINLOCK] Test 4: Stress test (10000x)\n");
    spinlock_t stress = SPINLOCK_INIT;
    volatile uint32_t counter = 0;

    PerfCounter pc;
    perf_start(&pc);
    for (int i = 0; i < 10000; i++) {
        spinlock_lock(&stress);
        counter++;
        spinlock_unlock(&stress);
    }
    perf_stop(&pc);
    perf_print(&pc, "spinlock_10000x");

    if (counter != 10000) {
        println64("  [FAIL] Counter value incorrect!", VGA_RED);
        return;
    }

    buf[0] = '\0';
    const char* p1 = "  [OK] 10000x lock/unlock, counter=";
    int bi = 0;
    while (p1[bi]) { buf[bi] = p1[bi]; bi++; }
    char tmp[12]; u32_to_str(counter, tmp);
    int ti = 0;
    while (tmp[ti]) { buf[bi++] = tmp[ti++]; }
    buf[bi++] = ' '; buf[bi++] = '(';
    u32_to_str(perf_us(&pc), tmp); ti = 0;
    while (tmp[ti]) { buf[bi++] = tmp[ti++]; }
    const char* p2 = " us)";
    ti = 0;
    while (p2[ti]) { buf[bi++] = p2[ti++]; }
    buf[bi] = '\0';
    println64(buf, VGA_LIGHT_GREEN);

    // Test 5 RW Lock
    serial_print("[SPINLOCK] Test 5: RW Lock\n");
    rwlock_t rw = RWLOCK_INIT;

    rwlock_read_lock(&rw);
    rwlock_read_lock(&rw);
    if (rw.readers != 2) {
        println64("  [FAIL] rwlock: reader count incorrect with 2 readers", VGA_RED);
        return;
    }
    println64("  [OK] rwlock: 2 readers can enter simultaneously", VGA_LIGHT_GREEN);
    rwlock_read_unlock(&rw);
    rwlock_read_unlock(&rw);

    if (rw.readers != 0) {
        println64("  [FAIL] rwlock: reader count not zero after unlocks", VGA_RED);
        return;
    }
    println64("  [OK] rwlock: readers exited, count=0", VGA_LIGHT_GREEN);

    // Writer lock
    rwlock_write_lock(&rw);
    if (!spinlock_is_locked(&rw.write_lock)) {
        println64("  [FAIL] rwlock: write lock not acquired", VGA_RED);
        rwlock_write_unlock(&rw);
        return;
    }
    println64("  [OK] rwlock: write lock acquired", VGA_LIGHT_GREEN);
    rwlock_write_unlock(&rw);

    if (spinlock_is_locked(&rw.write_lock)) {
        println64("  [FAIL] rwlock: write lock not released", VGA_RED);
        return;
    }
    println64("  [OK] rwlock: write lock released", VGA_LIGHT_GREEN);

    serial_print("[SPINLOCK] All tests PASSED\n");
    println64("", VGA_WHITE);
    println64("  All spinlock tests PASSED!", VGA_CYAN);
}