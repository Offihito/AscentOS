// pmm.h — Physical Memory Manager
// Bitmap-tabanlı 4KB frame tahsisçi.
// Döngüsel bağımlılık yok: VMM veya heap'e bağımlı değil.
#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE       4096ULL
#define PAGE_SHIFT      12

// E820 bellek haritası girdisi (multiboot2 uyumlu)
typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;          // 1 = kullanılabilir RAM
    uint32_t acpi_extended;
} __attribute__((packed)) pmm_mmap_entry_t;

#define MMAP_TYPE_USABLE    1
#define MMAP_TYPE_RESERVED  2
#define MMAP_TYPE_ACPI_REC  3
#define MMAP_TYPE_ACPI_NVS  4
#define MMAP_TYPE_BAD       5

// -----------------------------------------------------------------------
// Başlatma
// -----------------------------------------------------------------------

// mmap: E820 tablosu,  count: giriş sayısı
// kernel_end: çekirdeğin bittiği fiziksel adres (bitmap kendisi buraya yerleşir)
void pmm_init(pmm_mmap_entry_t *mmap, uint32_t count, uint64_t kernel_end_phys);

// -----------------------------------------------------------------------
// Frame tahsisi
// -----------------------------------------------------------------------

// Tek 4KB fiziksel frame tahsis et. Döner: fiziksel adres veya 0 (hata).
uint64_t pmm_alloc_frame(void);

// Frame serbest bırak (fiziksel adres).
void pmm_free_frame(uint64_t phys);

// -----------------------------------------------------------------------
// İstatistikler
// -----------------------------------------------------------------------
uint64_t pmm_total_frames(void);
uint64_t pmm_free_frames(void);
uint64_t pmm_used_frames(void);
uint64_t pmm_total_bytes(void);
uint64_t pmm_free_bytes(void);

void pmm_print_stats(void);

#endif // PMM_H