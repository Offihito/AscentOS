// cpu64.c — AscentOS 64-bit CPU Alt Sistemi
// SSE başlatma, CPUID sorgulama ve I/O port yardımcıları.

#include <stdint.h>
#include "cpu64.h"

// serial_print dışarıdan gelir (kernel64.c / serial64.c)
extern void serial_print(const char* s);

// ============================================================================
// SSE Başlatma
// CR0: EM biti temizle, MP biti koy, TS temizle
// CR4: OSFXSR + OSXMMEXCPT bitlerini aç
// ============================================================================
void sse_init(void) {
    uint64_t cr0, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);   // CR0.EM = 0  (x87 emülasyonu kapat)
    cr0 |=  (1ULL << 1);   // CR0.MP = 1  (Monitor Coprocessor)
    cr0 &= ~(1ULL << 3);   // CR0.TS = 0  (Task Switched temizle)
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);    // CR4.OSFXSR    = 1
    cr4 |= (1ULL << 10);   // CR4.OSXMMEXCPT= 1
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

    serial_print("[SSE] OK\n");
}

// ============================================================================
// CPUID — İşlemci üretici adını al
// v: en az 13 bayt (12 karakter + null)
// ============================================================================
void get_cpu_info(char* v) {
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid"
        : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
        : "a"(0));
    // EBX → EDX → ECX sırası: "GenuineIntel" / "AuthenticAMD" vb.
    *(uint32_t*)(v + 0) = b;
    *(uint32_t*)(v + 4) = d;
    *(uint32_t*)(v + 8) = c;
    v[12] = '\0';
}

// ============================================================================
// uint64_to_hex — 64-bit sayıyı "0xXXXXXXXXXXXXXXXX" biçimine çevirir
// buf: en az 19 bayt (2 + 16 + null)
// ============================================================================
void uint64_to_hex(uint64_t n, char* buf) {
    const char* h = "0123456789ABCDEF";
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = h[(n >> (60 - i * 4)) & 0xF];
    buf[18] = '\0';
}

// ============================================================================
// cpu_get_cr2 — Page Fault'un oluştuğu sanal adresi döndürür (CR2 kaydedicisi)
// Yalnızca #PF (exception #14) handler içinden çağrılmalıdır;
// başka zamanlarda değer tanımsızdır.
// panic64.c bu fonksiyonu kullanarak hata adresini ekrana/serial'a yazar.
// ============================================================================
uint64_t cpu_get_cr2(void) {
    uint64_t val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

// ============================================================================
// cpu_get_features — CPUID ile desteklenen CPU özelliklerini sorgular.
// Dönüş değeri CPU_FEAT_* bayraklarının OR'lanmış halidir (cpu64.h).
//
// Örnek kullanım:
//   uint32_t f = cpu_get_features();
//   if (f & CPU_FEAT_AVX) { ... }
// ============================================================================
uint32_t cpu_get_features(void) {
    uint32_t a, b, c, d;
    uint32_t flags = 0;

    // ── CPUID leaf 1: standart özellikler ────────────────────────────────
    __asm__ volatile ("cpuid"
        : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
        : "a"(1));

    if (d & (1U <<  0)) flags |= CPU_FEAT_FPU;
    if (d & (1U <<  4)) flags |= CPU_FEAT_TSC;
    if (d & (1U <<  6)) flags |= CPU_FEAT_PAE;
    if (d & (1U << 23)) flags |= CPU_FEAT_MMX;
    if (d & (1U << 25)) flags |= CPU_FEAT_SSE;
    if (d & (1U << 26)) flags |= CPU_FEAT_SSE2;
    if (c & (1U <<  0)) flags |= CPU_FEAT_SSE3;
    if (c & (1U <<  9)) flags |= CPU_FEAT_SSSE3;
    if (c & (1U << 19)) flags |= CPU_FEAT_SSE41;
    if (c & (1U << 20)) flags |= CPU_FEAT_SSE42;
    if (c & (1U << 28)) flags |= CPU_FEAT_AVX;
    if (c & (1U << 25)) flags |= CPU_FEAT_AES;
    if (c & (1U << 30)) flags |= CPU_FEAT_RDRAND;

    // ── CPUID leaf 0x80000001: uzatılmış özellikler (Long Mode) ──────────
    uint32_t max_ext;
    __asm__ volatile ("cpuid"
        : "=a"(max_ext), "=b"(b), "=c"(c), "=d"(d)
        : "a"(0x80000000U));

    if (max_ext >= 0x80000001U) {
        __asm__ volatile ("cpuid"
            : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
            : "a"(0x80000001U));
        if (d & (1U << 29)) flags |= CPU_FEAT_LONG;
    }

    return flags;
}

// ============================================================================
// cpu_get_model_name — CPUID leaf 0x80000002-4 ile tam model adını alır.
// out: en az 49 bayt tampon.
// Desteklenmiyorsa out[0] = '\0' döner.
//
// Örnek çıktı: "Intel(R) Core(TM) i7-9700K CPU @ 3.60GHz"
// ============================================================================
void cpu_get_model_name(char* out) {
    uint32_t max_ext;
    uint32_t a, b, c, d;

    __asm__ volatile ("cpuid"
        : "=a"(max_ext), "=b"(b), "=c"(c), "=d"(d)
        : "a"(0x80000000U));

    if (max_ext < 0x80000004U) {
        out[0] = '\0';
        return;
    }

    // Leaf 0x80000002, 0x80000003, 0x80000004 → 48 bayt model string
    for (int i = 0; i < 3; i++) {
        __asm__ volatile ("cpuid"
            : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
            : "a"(0x80000002U + (uint32_t)i));
        *(uint32_t*)(out + i * 16 +  0) = a;
        *(uint32_t*)(out + i * 16 +  4) = b;
        *(uint32_t*)(out + i * 16 +  8) = c;
        *(uint32_t*)(out + i * 16 + 12) = d;
    }
    out[48] = '\0';

    // Baştaki boşlukları temizle (bazı CPU'lar boşlukla başlar)
    char* p = out;
    while (*p == ' ') p++;
    if (p != out) {
        char* dst = out;
        while (*p) *dst++ = *p++;
        *dst = '\0';
    }
}

// ============================================================================
// cpu_get_stepping — CPUID leaf 1 ile Family/Model/Stepping bilgisini alır.
//
// Intel/AMD standart EAX formatı:
//   [3:0]   Stepping ID
//   [7:4]   Model
//   [11:8]  Family
//   [13:12] CPU Type
//   [19:16] Extended Model  → gerçek model  = Model + (ExtModel << 4)
//   [27:20] Extended Family → gerçek family = Family + ExtFamily (family==0xF)
//
// Örnek çıktı (Ryzen 5 7600):
//   Family=25  Model=97  Stepping=2  → Zen4 mimarisi
// ============================================================================
void cpu_get_stepping(CPUStepping* out) {
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid"
        : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
        : "a"(1));

    out->stepping = (uint8_t)( a        & 0xF);
    uint8_t  base_model  = (uint8_t)((a >>  4) & 0xF);
    uint16_t base_family = (uint16_t)((a >>  8) & 0xF);
    out->cpu_type        = (uint8_t)((a >> 12) & 0x3);
    uint8_t  ext_model   = (uint8_t)((a >> 16) & 0xF);
    uint8_t  ext_family  = (uint8_t)((a >> 20) & 0xFF);

    // Extended model her zaman eklenir (Intel/AMD ortak kural)
    out->model = base_model + (ext_model << 4);

    // Extended family yalnızca base_family == 0xF ise eklenir
    if (base_family == 0xF)
        out->family = (uint16_t)(base_family + ext_family);
    else
        out->family = base_family;
}

// ============================================================================
// cpu_get_cache_info — L1/L2/L3 önbellek boyutlarını CPUID ile sorgular.
//
// Intel → CPUID leaf 4 (Deterministic Cache Parameters)
// AMD   → CPUID leaf 0x80000005 (L1) + 0x80000006 (L2/L3)
//
// Her iki yöntem de denenir; bulunamazsa ilgili alan 0 kalır.
// ============================================================================
void cpu_get_cache_info(CacheInfo* out) {
    uint32_t a, b, c, d;
    out->l1d_kb = 0;
    out->l1i_kb = 0;
    out->l2_kb  = 0;
    out->l3_kb  = 0;

    // ── Vendor'ı tespit et ────────────────────────────────────────────────
    char vendor[13];
    get_cpu_info(vendor);

    // "AuthenticAMD" mi?
    int is_amd = (vendor[0] == 'A' && vendor[1] == 'u' &&
                  vendor[2] == 't' && vendor[3] == 'h');

    if (is_amd) {
        // ── AMD: leaf 0x80000005 → L1 ────────────────────────────────────
        uint32_t max_ext;
        __asm__ volatile ("cpuid"
            : "=a"(max_ext), "=b"(b), "=c"(c), "=d"(d)
            : "a"(0x80000000U));

        if (max_ext >= 0x80000005U) {
            __asm__ volatile ("cpuid"
                : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                : "a"(0x80000005U));
            // ECX[31:24] = L1D boyutu (KB)
            // EDX[31:24] = L1I boyutu (KB)
            out->l1d_kb = (c >> 24) & 0xFF;
            out->l1i_kb = (d >> 24) & 0xFF;
        }

        // ── AMD: leaf 0x80000006 → L2 / L3 ──────────────────────────────
        if (max_ext >= 0x80000006U) {
            __asm__ volatile ("cpuid"
                : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                : "a"(0x80000006U));
            // ECX[31:16] = L2 boyutu (KB)
            // EDX[31:18] = L3 boyutu (×512 KB)
            out->l2_kb = (c >> 16) & 0xFFFF;
            uint32_t l3_raw = (d >> 18) & 0x3FFF;
            out->l3_kb = l3_raw * 512;
        }

    } else {
        // ── Intel: leaf 4 (Deterministic Cache Parameters) ───────────────
        // Alt leaf'leri sırayla sorgula; cache_type=0 gelince dur.
        // cache_type: 1=Data, 2=Instruction, 3=Unified
        for (uint32_t subleaf = 0; subleaf < 16; subleaf++) {
            __asm__ volatile ("cpuid"
                : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                : "a"(4), "c"(subleaf));

            uint32_t cache_type  = a & 0x1F;
            if (cache_type == 0) break;            // liste bitti

            uint32_t cache_level = (a >> 5) & 0x7;
            uint32_t line_size   = (b & 0xFFF) + 1;
            uint32_t partitions  = ((b >> 12) & 0x3FF) + 1;
            uint32_t ways        = ((b >> 22) & 0x3FF) + 1;
            uint32_t sets        = c + 1;
            uint32_t size_kb     = (ways * partitions * line_size * sets) / 1024;

            if (cache_level == 1 && cache_type == 1) out->l1d_kb = size_kb;
            if (cache_level == 1 && cache_type == 2) out->l1i_kb = size_kb;
            if (cache_level == 2) out->l2_kb = size_kb;
            if (cache_level == 3) out->l3_kb = size_kb;
        }
    }
}

// ============================================================================
// cpu_get_freq_estimate — TSC + PIT ile yaklaşık CPU frekansını ölçer.
//
// Yöntem:
//   1. PIT (8253/8254) kanal 2'yi 50ms sayacı olarak kur
//   2. Başlangıçta RDTSC oku
//   3. PIT sayacı sıfırlanana kadar bekle (~50ms)
//   4. Bitişteki RDTSC ile fark → MHz hesapla
//
// Doğruluk: ±2-5 MHz (PIT'in hassasiyetine bağlı)
// Kernel boot sırasında timer interrupt'ı varsa sonuç biraz kayabilir.
// ============================================================================
uint32_t cpu_get_freq_estimate(void) {
    // PIT Kanal 2, Mode 0 (One-shot), binary
    // Frekans: 1.193182 MHz → 50ms için: 1193182 * 50 / 1000 = 59659 tik
    const uint16_t PIT_CH2   = 0x42;
    const uint16_t PIT_CMD   = 0x43;
    const uint16_t PIT_GATE  = 0x61;

    // Kanal 2'yi etkinleştir (bit0=1), hoparlörü kapat (bit1=0)
    uint8_t gate = inb(PIT_GATE);
    outb(PIT_GATE, (gate & 0xFC) | 0x01);

    // Kanal 2: Mode 0, LSB+MSB, binary
    outb(PIT_CMD, 0xB0);
    outb(PIT_CH2, 0x0B);   // LSB: 59659 = 0xE90B
    outb(PIT_CH2, 0xE9);   // MSB

    // RDTSC başlangıç
    uint32_t lo1, hi1, lo2, hi2;
    __asm__ volatile ("rdtsc" : "=a"(lo1), "=d"(hi1));

    // PIT OUT biti (bit5) sıfırlanana kadar bekle → ~50ms
    while (inb(PIT_GATE) & 0x20);

    // RDTSC bitiş
    __asm__ volatile ("rdtsc" : "=a"(lo2), "=d"(hi2));

    // Kanal 2'yi eski haline getir
    outb(PIT_GATE, gate);

    uint64_t tsc1 = ((uint64_t)hi1 << 32) | lo1;
    uint64_t tsc2 = ((uint64_t)hi2 << 32) | lo2;
    uint64_t delta = tsc2 - tsc1;

    // delta tik / 50ms → MHz = delta / 50000
    return (uint32_t)(delta / 50000);
}

// ============================================================================
// Performans ölçüm fonksiyonları — PerfCounter
//
// Kullanım:
//   PerfCounter pc;
//   perf_start(&pc);
//   ... ölçülecek kod ...
//   perf_stop(&pc);
//   perf_print(&pc, "islem_adi");
//
// Cycle → ns dönüşümü için cpu_get_freq_estimate() çağrılır.
// Frekans tahmininin ±5 MHz hata payı olduğundan ns değeri yaklaşıktır.
// ============================================================================

// ── Ölçümü başlat ────────────────────────────────────────────────────────────
// Memory barrier + RDTSC: önceki tüm yazmaların tamamlanmasını garantiler.
void perf_start(PerfCounter* pc) {
    pc->end_tsc   = 0;
    pc->elapsed   = 0;
    pc->cpu_mhz   = 0;
    __asm__ volatile ("" ::: "memory");   // derleyici barrier
    pc->start_tsc = cpu_rdtsc();
}

// ── Ölçümü bitir ─────────────────────────────────────────────────────────────
void perf_stop(PerfCounter* pc) {
    __asm__ volatile ("" ::: "memory");   // derleyici barrier
    pc->end_tsc = cpu_rdtsc();
    pc->elapsed = (pc->end_tsc > pc->start_tsc)
                  ? pc->end_tsc - pc->start_tsc
                  : 0;
    pc->cpu_mhz = cpu_get_freq_estimate();
    if (pc->cpu_mhz == 0) pc->cpu_mhz = 1000;
}

// ── Geçen cycle sayısı ───────────────────────────────────────────────────────
uint64_t perf_cycles(const PerfCounter* pc) {
    return pc->elapsed;
}

// ── Yaklaşık nanosaniye ──────────────────────────────────────────────────────
// ns = cycles / (MHz / 1000) = cycles * 1000 / MHz
uint64_t perf_ns(const PerfCounter* pc) {
    if (pc->cpu_mhz == 0) return 0;
    return (pc->elapsed * 1000ULL) / (uint64_t)pc->cpu_mhz;
}

// ── Yaklaşık mikrosaniye ─────────────────────────────────────────────────────
uint32_t perf_us(const PerfCounter* pc) {
    uint64_t ns = perf_ns(pc);
    return (uint32_t)(ns / 1000ULL);
}

// ── Sonucu serial porta yaz ──────────────────────────────────────────────────
// Örnek çıktı:
//   [PERF] memcpy_test : 12450 cycles  ~4975 ns  ~4 us  @2500 MHz
void perf_print(const PerfCounter* pc, const char* label) {
    // Küçük yardımcı: uint64 → decimal string
    static const char* DEC = "0123456789";

    // Sayıyı string'e çevir (geriye doğru yaz, sonra çevir)
    char buf[24];
    int  bi;

    serial_print("[PERF] ");
    if (label) serial_print(label);
    serial_print(" : ");

    // Cycles
    {
        uint64_t v = pc->elapsed; bi = 0;
        if (!v) { buf[bi++] = '0'; }
        else { while (v) { buf[bi++] = DEC[v % 10]; v /= 10; } }
        // Ters çevir
        for (int i = 0, j = bi-1; i < j; i++, j--) {
            char t = buf[i]; buf[i] = buf[j]; buf[j] = t;
        }
        buf[bi] = '\0';
        serial_print(buf);
        serial_print(" cycles  ~");
    }

    // Nanosaniye
    {
        uint64_t v = perf_ns(pc); bi = 0;
        if (!v) { buf[bi++] = '0'; }
        else { while (v) { buf[bi++] = DEC[v % 10]; v /= 10; } }
        for (int i = 0, j = bi-1; i < j; i++, j--) {
            char t = buf[i]; buf[i] = buf[j]; buf[j] = t;
        }
        buf[bi] = '\0';
        serial_print(buf);
        serial_print(" ns  ~");
    }

    // Mikrosaniye
    {
        uint32_t v = perf_us(pc); bi = 0;
        if (!v) { buf[bi++] = '0'; }
        else { while ((uint64_t)v) { buf[bi++] = DEC[v % 10]; v /= 10; } }
        for (int i = 0, j = bi-1; i < j; i++, j--) {
            char t = buf[i]; buf[i] = buf[j]; buf[j] = t;
        }
        buf[bi] = '\0';
        serial_print(buf);
        serial_print(" us  @");
    }

    // MHz
    {
        uint32_t v = pc->cpu_mhz; bi = 0;
        if (!v) { buf[bi++] = '0'; }
        else { while (v) { buf[bi++] = DEC[v % 10]; v /= 10; } }
        for (int i = 0, j = bi-1; i < j; i++, j--) {
            char t = buf[i]; buf[i] = buf[j]; buf[j] = t;
        }
        buf[bi] = '\0';
        serial_print(buf);
        serial_print(" MHz\n");
    }
}