// pmm.c — Physical Memory Manager
// Bitmap tabanlı; her bit bir 4KB frame'i temsil eder.
// Bağımlılık: hiçbir heap veya VMM fonksiyonu çağrılmaz.
//             Sadece serial_print + println64/print_str64 kullanılır.

#include "pmm.h"
#include <stdint.h>
#include <stddef.h>

// -----------------------------------------------------------------------
// Dış bildirimler (kernel tarafından sağlanır)
// -----------------------------------------------------------------------
extern void serial_print (const char *s);
extern void print_str64  (const char *s, uint8_t color);
extern void println64    (const char *s, uint8_t color);

// -----------------------------------------------------------------------
// Renk sabitleri
// -----------------------------------------------------------------------
#define CLR_GREEN   0x0A
#define CLR_YELLOW  0x0E
#define CLR_RED     0x0C
#define CLR_CYAN    0x03
#define CLR_WHITE   0x0F

// -----------------------------------------------------------------------
// Yardımcı: uint64_t → onluk string (statik tampon, tek kullanım)
// -----------------------------------------------------------------------
static void u64_to_str(uint64_t n, char *buf, int buf_size) {
    if (buf_size < 2) return;
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    int i = 0;
    char tmp[24];
    while (n && i < (int)sizeof(tmp) - 1) { tmp[i++] = '0' + (n % 10); n /= 10; }
    int out = 0;
    while (i-- && out < buf_size - 1) buf[out++] = tmp[i];
    buf[out] = '\0';
}

// -----------------------------------------------------------------------
// Bitmap depolaması
// Desteklenen maksimum fiziksel bellek: 64 GB
// Bitmap boyutu: 64G / 4K / 8 = 2 MB (statik, BSS'te)
// -----------------------------------------------------------------------
#define MAX_PHYS_MEMORY     (64ULL * 1024 * 1024 * 1024)
#define BITMAP_BYTES        (MAX_PHYS_MEMORY / PAGE_SIZE / 8)

static uint8_t  s_bitmap[BITMAP_BYTES];
static uint64_t s_total_frames = 0;
static uint64_t s_free_frames  = 0;
static uint64_t s_used_frames  = 0;

// Son başarılı aramanın bittiği frame (next-fit hızlandırması)
static uint64_t s_next_fit_hint = 0;

static int s_initialized = 0;

// -----------------------------------------------------------------------
// Bitmap yardımcıları  (inline → hız)
// -----------------------------------------------------------------------
static inline void  bm_set  (uint64_t f) { s_bitmap[f >> 3] |=  (uint8_t)(1u << (f & 7)); }
static inline void  bm_clr  (uint64_t f) { s_bitmap[f >> 3] &= (uint8_t)~(1u << (f & 7)); }
static inline int   bm_test (uint64_t f) { return (s_bitmap[f >> 3] >> (f & 7)) & 1; }

// -----------------------------------------------------------------------
// Dahili: bir bellek bölgesini serbest olarak işaretle
// -----------------------------------------------------------------------
static void pmm_mark_free(uint64_t base, uint64_t length) {
    uint64_t start = base >> PAGE_SHIFT;
    uint64_t end   = (base + length) >> PAGE_SHIFT;
    if (end > s_total_frames) end = s_total_frames;
    for (uint64_t i = start; i < end; i++) {
        if (bm_test(i)) {          // sadece önceden "kullanılmış" olanları serbest bırak
            bm_clr(i);
            s_free_frames++;
            s_used_frames--;
        }
    }
}

// Dahili: bir bellek bölgesini kullanılmış olarak işaretle
static void pmm_mark_used(uint64_t base, uint64_t length) {
    uint64_t start = base >> PAGE_SHIFT;
    uint64_t end   = (base + length + PAGE_SIZE - 1) >> PAGE_SHIFT;
    if (end > s_total_frames) end = s_total_frames;
    for (uint64_t i = start; i < end; i++) {
        if (!bm_test(i)) {
            bm_set(i);
            s_free_frames--;
            s_used_frames++;
        }
    }
}

// -----------------------------------------------------------------------
// pmm_init
// -----------------------------------------------------------------------
void pmm_init(pmm_mmap_entry_t *mmap, uint32_t count, uint64_t kernel_end_phys) {

    // Bitmap'i tamamen dolu (kullanılmış) olarak başlat
    for (uint64_t i = 0; i < BITMAP_BYTES; i++) s_bitmap[i] = 0xFF;

    // En yüksek fiziksel adresi hesapla
    uint64_t max_addr = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (mmap[i].type == MMAP_TYPE_USABLE) {
            uint64_t end = mmap[i].base + mmap[i].length;
            if (end > max_addr) max_addr = end;
        }
    }
    if (max_addr > MAX_PHYS_MEMORY) max_addr = MAX_PHYS_MEMORY;

    s_total_frames = max_addr >> PAGE_SHIFT;
    s_free_frames  = 0;
    s_used_frames  = s_total_frames;

    // Kullanılabilir bölgeleri serbest bırak
    for (uint32_t i = 0; i < count; i++) {
        if (mmap[i].type == MMAP_TYPE_USABLE) {
            uint64_t base = mmap[i].base;
            uint64_t len  = mmap[i].length;
            if (base >= MAX_PHYS_MEMORY) continue;
            if (base + len > MAX_PHYS_MEMORY) len = MAX_PHYS_MEMORY - base;
            pmm_mark_free(base, len);
        }
    }

    // Korunan bölgeleri yeniden rezerve et
    // 1. İlk 1 MB (BIOS, IVT, vb.)
    pmm_mark_used(0, 0x100000);

    // 2. Çekirdek kodu + veri + bitmap (kernel_end_phys kadar)
    pmm_mark_used(0x100000, kernel_end_phys - 0x100000 + PAGE_SIZE);

    // 3. Bitmap'in kendisi (BSS içinde, çekirdek kapsamında ise bu otomatik)
    //    Güvenlik için bitmap'in fiziksel adresini de işaretle:
    uint64_t bm_phys = (uint64_t)(uintptr_t)s_bitmap;
    // Eğer higher-half kernel ise fiziksel adrese dönüştür
    // (basit kural: 0xFFFF... üstüyse yüksek yarı)
    if (bm_phys >= 0xFFFFFFFF80000000ULL)
        bm_phys = bm_phys - 0xFFFFFFFF80000000ULL + 0x100000ULL;
    pmm_mark_used(bm_phys, BITMAP_BYTES);

    s_next_fit_hint = 0;
    s_initialized   = 1;

    // Durum çıktısı
    char buf[24];
    uint64_t total_mb = (s_total_frames * PAGE_SIZE) >> 20;
    print_str64("[PMM] Baslatildi: ", CLR_GREEN);
    u64_to_str(total_mb, buf, sizeof(buf));
    print_str64(buf, CLR_YELLOW);
    print_str64(" MB, serbest: ", CLR_GREEN);
    u64_to_str((s_free_frames * PAGE_SIZE) >> 20, buf, sizeof(buf));
    print_str64(buf, CLR_YELLOW);
    println64(" MB", CLR_GREEN);

    serial_print("[PMM] init OK\n");
}

// -----------------------------------------------------------------------
// pmm_alloc_frame  — Next-fit algoritması
// -----------------------------------------------------------------------
uint64_t pmm_alloc_frame(void) {
    if (!s_initialized || s_free_frames == 0) return 0;

    // Next-fit: s_next_fit_hint'ten başla, iki tur dene
    for (int pass = 0; pass < 2; pass++) {
        uint64_t start = (pass == 0) ? s_next_fit_hint : 0;
        uint64_t end   = (pass == 0) ? s_total_frames  : s_next_fit_hint;

        for (uint64_t f = start; f < end; f++) {
            if (!bm_test(f)) {
                bm_set(f);
                s_free_frames--;
                s_used_frames++;
                s_next_fit_hint = f + 1;
                if (s_next_fit_hint >= s_total_frames) s_next_fit_hint = 0;
                return f << PAGE_SHIFT;  // fiziksel adres
            }
        }
    }

    serial_print("[PMM] HATA: Bos frame bulunamadi!\n");
    return 0;
}

// -----------------------------------------------------------------------
// pmm_free_frame
// -----------------------------------------------------------------------
void pmm_free_frame(uint64_t phys) {
    if (!s_initialized) return;
    uint64_t f = phys >> PAGE_SHIFT;
    if (f >= s_total_frames) return;

    if (!bm_test(f)) {
        serial_print("[PMM] UYARI: Zaten serbest frame serbest birakilmaya calisiliyor!\n");
        return;
    }

    bm_clr(f);
    s_free_frames++;
    s_used_frames--;
}

// -----------------------------------------------------------------------
// İstatistik getericileri
// -----------------------------------------------------------------------
uint64_t pmm_total_frames(void) { return s_total_frames; }
uint64_t pmm_free_frames (void) { return s_free_frames;  }
uint64_t pmm_used_frames (void) { return s_used_frames;  }
uint64_t pmm_total_bytes (void) { return s_total_frames * PAGE_SIZE; }
uint64_t pmm_free_bytes  (void) { return s_free_frames  * PAGE_SIZE; }

// -----------------------------------------------------------------------
// pmm_print_stats
// -----------------------------------------------------------------------
void pmm_print_stats(void) {
    char buf[24];
    println64("=== PMM Istatistikleri ===", CLR_CYAN);

    print_str64("  Toplam bellek : ", CLR_WHITE);
    u64_to_str(pmm_total_bytes() >> 20, buf, sizeof(buf));
    print_str64(buf, CLR_GREEN); println64(" MB", CLR_GREEN);

    print_str64("  Kullanilan    : ", CLR_WHITE);
    u64_to_str((s_used_frames * PAGE_SIZE) >> 20, buf, sizeof(buf));
    print_str64(buf, CLR_YELLOW); println64(" MB", CLR_YELLOW);

    print_str64("  Serbest       : ", CLR_WHITE);
    u64_to_str(pmm_free_bytes() >> 20, buf, sizeof(buf));
    print_str64(buf, CLR_GREEN); println64(" MB", CLR_GREEN);

    print_str64("  Toplam frame  : ", CLR_WHITE);
    u64_to_str(s_total_frames, buf, sizeof(buf));
    println64(buf, CLR_GREEN);

    print_str64("  Bos frame     : ", CLR_WHITE);
    u64_to_str(s_free_frames, buf, sizeof(buf));
    println64(buf, CLR_GREEN);
}