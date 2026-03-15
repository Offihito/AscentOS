#ifndef CPU64_H
#define CPU64_H

#include <stdint.h>

// ── I/O Port erişimi ──────────────────────────────────────────────────────────
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ── CPU özellik bayrakları (cpu_get_features() dönüş değeri için) ─────────────
#define CPU_FEAT_FPU   (1 << 0)   // x87 FPU
#define CPU_FEAT_TSC   (1 << 1)   // RDTSC
#define CPU_FEAT_PAE   (1 << 2)   // Physical Address Extension
#define CPU_FEAT_MMX   (1 << 3)   // MMX
#define CPU_FEAT_SSE   (1 << 4)   // SSE
#define CPU_FEAT_SSE2  (1 << 5)   // SSE2
#define CPU_FEAT_SSE3  (1 << 6)   // SSE3
#define CPU_FEAT_SSSE3 (1 << 7)   // Supplemental SSE3
#define CPU_FEAT_SSE41 (1 << 8)   // SSE4.1
#define CPU_FEAT_SSE42 (1 << 9)   // SSE4.2
#define CPU_FEAT_AVX   (1 << 10)  // AVX
#define CPU_FEAT_AES   (1 << 11)  // AES-NI
#define CPU_FEAT_RDRAND (1 << 12) // RDRAND (donanım RNG)
#define CPU_FEAT_LONG  (1 << 13)  // Long Mode (64-bit)

// ── Cache bilgisi (cpu_get_cache_info() için) ─────────────────────────────────
typedef struct {
    uint32_t l1d_kb;    // L1 Data cache (KB)
    uint32_t l1i_kb;    // L1 Instruction cache (KB)
    uint32_t l2_kb;     // L2 cache (KB)
    uint32_t l3_kb;     // L3 cache (KB), yoksa 0
} CacheInfo;

// ── CPU kimlik bilgisi (cpu_get_stepping() için) ──────────────────────────────
typedef struct {
    uint8_t  stepping;   // 0-15
    uint8_t  model;      // 0-15 (extended ile birleştirilmiş)
    uint16_t family;     // 0-15 (extended ile birleştirilmiş)
    uint8_t  cpu_type;   // 0=OEM, 1=OverDrive, 2=Dual
} CPUStepping;

// ── Fonksiyon bildirimleri ────────────────────────────────────────────────────
void     sse_init(void);
void     get_cpu_info(char* vendor_out);        // en az 13 bayt tampon
void     uint64_to_hex(uint64_t n, char* buf);  // en az 19 bayt tampon
uint64_t cpu_get_cr2(void);                     // Page Fault adresi (CR2)
uint32_t cpu_get_features(void);                // CPU özellik bayraklarını döndür
void     cpu_get_model_name(char* out);         // Tam model adı, en az 49 bayt tampon
void     cpu_get_cache_info(CacheInfo* out);    // L1/L2/L3 önbellek boyutları
uint32_t cpu_get_freq_estimate(void);           // Yaklaşık frekans (MHz)
void     cpu_get_stepping(CPUStepping* out);    // Family/Model/Stepping bilgisi

// ── Interrupt kontrolü ────────────────────────────────────────────────────────
static inline void cpu_enable_interrupts(void) {
    __asm__ volatile ("sti" ::: "memory");
}

static inline void cpu_disable_interrupts(void) {
    __asm__ volatile ("cli" ::: "memory");
}

// Mevcut interrupt durumunu kaydet ve kapat (kritik bölge başlangıcı)
// Kullanım: uint64_t flags = cpu_save_flags(); ... cpu_restore_flags(flags);
static inline uint64_t cpu_save_flags(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) :: "memory");
    __asm__ volatile ("cli" ::: "memory");
    return flags;
}

// Kaydedilen interrupt durumunu geri yükle (kritik bölge sonu)
static inline void cpu_restore_flags(uint64_t flags) {
    __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

// ── TLB yönetimi ─────────────────────────────────────────────────────────────
static inline void cpu_invlpg(uint64_t virtual_addr) {
    __asm__ volatile ("invlpg (%0)" :: "r"(virtual_addr) : "memory");
}

// ── Spinlock / busy-wait yardımcısı ──────────────────────────────────────────
// PAUSE: CPU'ya "spin-wait döngüsündeyim" sinyali verir.
// - Out-of-order execution'ı yavaşlatır → diğer thread'e bant genişliği bırakır
// - Hyperthreading'de karşı thread daha verimli çalışır
// - Güç tüketimini azaltır
// - Memory order violation pipeline flush'ını önler
static inline void cpu_relax(void) {
    __asm__ volatile ("pause" ::: "memory");
}

// ── TSC (Time Stamp Counter) ──────────────────────────────────────────────────
// RDTSC: CPU boot'undan bu yana geçen clock cycle sayısını okur.
static inline uint64_t cpu_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// NOT: rdtscp (-cpu qemu64'te desteklenmez) kullanılmıyor.
// Hassas ölçüm için rdtsc öncesi/sonrası memory barrier ekliyoruz.

// ── Performans ölçüm yapısı ───────────────────────────────────────────────────
typedef struct {
    uint64_t start_tsc;    // Ölçüm başlangıç TSC değeri
    uint64_t end_tsc;      // Ölçüm bitiş TSC değeri
    uint64_t elapsed;      // end_tsc - start_tsc (cycle cinsinden)
    uint32_t cpu_mhz;      // Ölçüm anındaki CPU frekansı (MHz) — ns hesabı için
} PerfCounter;

// ── Performans ölçüm fonksiyonları ───────────────────────────────────────────
void     perf_start(PerfCounter* pc);            // Ölçümü başlat
void     perf_stop(PerfCounter* pc);             // Ölçümü bitir
uint64_t perf_cycles(const PerfCounter* pc);     // Geçen cycle sayısı
uint64_t perf_ns(const PerfCounter* pc);         // Yaklaşık nanosaniye
uint32_t perf_us(const PerfCounter* pc);         // Yaklaşık mikrosaniye
void     perf_print(const PerfCounter* pc,       // Sonucu serial'a yaz
                    const char* label);

#endif // CPU64_H