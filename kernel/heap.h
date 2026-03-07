// heap.h — Kernel Dynamic Memory Allocator
// VMM üzerinden sayfa alarak büyüyen, birleştirme (coalescing) destekli heap.
// PMM veya VMM'ye doğrudan bağımlılık yoktur; sayfa sağlayan bir callback kullanır.
#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

// -----------------------------------------------------------------------
// Yapılandırma
// -----------------------------------------------------------------------
#define HEAP_VIRT_BASE      0x0000000001000000ULL  // 16 MB'dan başla
#define HEAP_MAX_SIZE       (128ULL * 1024 * 1024) // 128 MB üst sınır
#define HEAP_GROW_PAGES     16                     // Her genişlemede 16 sayfa (64 KB)
#define HEAP_MIN_SPLIT      64                     // Bölme için minimum artık bayt

// -----------------------------------------------------------------------
// Başlatma
// -----------------------------------------------------------------------

// heap'i başlat; sayfa allocator callback'ini kaydet.
// page_alloc(count) → fiziksel frame başı veya 0 (hata)
// page_free(phys, count) → frame'leri serbest bırak
typedef uint64_t (*heap_page_alloc_fn)(uint64_t count);
typedef void     (*heap_page_free_fn)(uint64_t phys, uint64_t count);

void heap_init(heap_page_alloc_fn palloc, heap_page_free_fn pfree);

// -----------------------------------------------------------------------
// Birincil arayüz
// -----------------------------------------------------------------------
void *kmalloc (size_t size);
void  kfree   (void *ptr);
void *krealloc(void *ptr, size_t new_size);
void *kcalloc (size_t nmemb, size_t size);

// -----------------------------------------------------------------------
// GUI uyumluluk takma adları
// -----------------------------------------------------------------------
static inline void *malloc_gui(uint64_t size) { return kmalloc((size_t)size); }
static inline void  free_gui  (void *ptr)      { kfree(ptr); }

// -----------------------------------------------------------------------
// Geriye dönük uyumluluk
// -----------------------------------------------------------------------
void init_memory_unified(void);  // heap_init() için ince sarmalayıcı
void init_memory64(void);
void init_memory_gui(void);

// -----------------------------------------------------------------------
// İstatistikler
// -----------------------------------------------------------------------
typedef struct {
    uint64_t total_allocs;
    uint64_t total_frees;
    uint64_t live_allocs;       // allocs - frees
    uint64_t bytes_live;        // şu an kullanılan bayt
    uint64_t bytes_peak;        // en yüksek kullanım
    uint64_t heap_pages;        // heap'e map edilmiş sayfa sayısı
    uint64_t coalesces;
    uint64_t splits;
} heap_stats_t;

heap_stats_t heap_get_stats(void);
void         heap_print_stats(void);

// show_memory_info: eski API uyumluluğu
void show_memory_info(void);

#endif // HEAP_H