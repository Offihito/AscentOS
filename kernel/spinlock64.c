// spinlock64.c — AscentOS 64-bit Spinlock Test & Debug
//
// Header-only inline fonksiyonların yanına test ve istatistik
// fonksiyonları burada implemente edilmiştir.

#include "spinlock64.h"
#include "cpu64.h"
#include <stdint.h>

extern void serial_print(const char* s);
extern void println64(const char* str, uint8_t color);
extern void print_str64(const char* str, uint8_t color);

// Renk sabitleri (vesa64.h'dan)
#define VGA_WHITE       0x0F
#define VGA_GREEN       0x02
#define VGA_CYAN        0x03
#define VGA_YELLOW      0x0E
#define VGA_RED         0x04
#define VGA_LIGHT_GREEN 0x0A
#define VGA_DARK_GRAY   0x08

// ── Küçük yardımcı: uint32 → string ──────────────────────────────────────────
static void u32_to_str(uint32_t v, char* buf) {
    if (!v) { buf[0] = '0'; buf[1] = '\0'; return; }
    char t[12]; int i = 0;
    while (v) { t[i++] = '0' + (v % 10); v /= 10; }
    int j = 0;
    while (--i >= 0) buf[j++] = t[i];
    buf[j] = '\0';
}

// ── spinlock_test — temel kilit/aç testi ─────────────────────────────────────
// commands64.c'deki cmd_spinlock() bu fonksiyonu çağırır.
void spinlock_test(void) {
    char buf[64];

    // ── Test 1: Temel lock/unlock ─────────────────────────────────────────
    serial_print("[SPINLOCK] Test 1: Temel lock/unlock\n");
    spinlock_t lock = SPINLOCK_INIT;

    if (spinlock_is_locked(&lock)) {
        serial_print("[SPINLOCK] FAIL: Init sonrasi kilitli olmamali!\n");
        println64("  [FAIL] Init: kilit bos olmali", VGA_RED);
        return;
    }
    println64("  [OK] Init: kilit bos", VGA_LIGHT_GREEN);

    spinlock_lock(&lock);
    if (!spinlock_is_locked(&lock)) {
        serial_print("[SPINLOCK] FAIL: lock() sonrasi kilitli olmali!\n");
        println64("  [FAIL] lock() sonrasi kilitli degil", VGA_RED);
        return;
    }
    println64("  [OK] lock(): kilit alindi", VGA_LIGHT_GREEN);

    spinlock_unlock(&lock);
    if (spinlock_is_locked(&lock)) {
        serial_print("[SPINLOCK] FAIL: unlock() sonrasi serbest olmali!\n");
        println64("  [FAIL] unlock() sonrasi hala kilitli", VGA_RED);
        return;
    }
    println64("  [OK] unlock(): kilit serbest", VGA_LIGHT_GREEN);

    // ── Test 2: trylock ───────────────────────────────────────────────────
    serial_print("[SPINLOCK] Test 2: trylock\n");

    int ok = spinlock_trylock(&lock);
    if (!ok) {
        println64("  [FAIL] trylock bos kilitte basarisiz", VGA_RED);
        return;
    }
    println64("  [OK] trylock: bos kilitte basarili", VGA_LIGHT_GREEN);

    int ok2 = spinlock_trylock(&lock);
    if (ok2) {
        println64("  [FAIL] trylock dolu kilitte basarili olmamali", VGA_RED);
        spinlock_unlock(&lock);
        return;
    }
    println64("  [OK] trylock: dolu kilitte basarisiz (dogru)", VGA_LIGHT_GREEN);
    spinlock_unlock(&lock);

    // ── Test 3: IRQ-safe lock/unlock ─────────────────────────────────────
    serial_print("[SPINLOCK] Test 3: IRQ-safe lock/unlock\n");

    uint64_t flags = spinlock_lock_irq(&lock);
    println64("  [OK] lock_irq: kilit alindi + IRQ kapatildi", VGA_LIGHT_GREEN);
    spinlock_unlock_irq(&lock, flags);
    println64("  [OK] unlock_irq: kilit serbest + IRQ geri yuklendi", VGA_LIGHT_GREEN);

    // ── Test 4: Tekrarlı lock/unlock (stres) ─────────────────────────────
    serial_print("[SPINLOCK] Test 4: Stres testi (10000x)\n");
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
        println64("  [FAIL] Sayac hatali!", VGA_RED);
        return;
    }

    buf[0] = '\0';
    // str_concat yerine doğrudan yaz
    const char* p1 = "  [OK] 10000x lock/unlock, sayac=";
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

    // ── Test 5: RW Lock ───────────────────────────────────────────────────
    serial_print("[SPINLOCK] Test 5: RW Lock\n");
    rwlock_t rw = RWLOCK_INIT;

    // İki okuyucu aynı anda girebilmeli
    rwlock_read_lock(&rw);
    rwlock_read_lock(&rw);
    if (rw.readers != 2) {
        println64("  [FAIL] rwlock: 2 okuyucu sayisi yanlis", VGA_RED);
        return;
    }
    println64("  [OK] rwlock: 2 okuyucu ayni anda girebilir", VGA_LIGHT_GREEN);
    rwlock_read_unlock(&rw);
    rwlock_read_unlock(&rw);

    if (rw.readers != 0) {
        println64("  [FAIL] rwlock: okuyucu cikisinda sayac sifirlanmadi", VGA_RED);
        return;
    }
    println64("  [OK] rwlock: okuyucular cikti, sayac=0", VGA_LIGHT_GREEN);

    // Yazıcı kilidi
    rwlock_write_lock(&rw);
    if (!spinlock_is_locked(&rw.write_lock)) {
        println64("  [FAIL] rwlock: yazici kilidi alinamadi", VGA_RED);
        rwlock_write_unlock(&rw);
        return;
    }
    println64("  [OK] rwlock: yazici kilidi aldi", VGA_LIGHT_GREEN);
    rwlock_write_unlock(&rw);

    if (spinlock_is_locked(&rw.write_lock)) {
        println64("  [FAIL] rwlock: yazici kilidi birakilmadi", VGA_RED);
        return;
    }
    println64("  [OK] rwlock: yazici kilidi birakti", VGA_LIGHT_GREEN);

    serial_print("[SPINLOCK] Tum testler BASARILI\n");
    println64("", VGA_WHITE);
    println64("  Tum spinlock testleri BASARILI!", VGA_CYAN);
}