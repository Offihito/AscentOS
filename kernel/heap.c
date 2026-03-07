// heap.c — Kernel Dynamic Memory Allocator
//
// Tasarım:
//   • Çift yönlü bağlı liste, blok başlık/altbilgi ile sınır etiketleme
//   • Birleştirme (coalescing): serbest bırakma anında O(1) komşu birleştirme
//   • Bölme (splitting): büyük serbest bloklardan küçük tahsisler
//   • Genişleme: heap dolunca VMM aracılığıyla yeni sayfalar eşlenir
//   • Sihirli sayı (magic) ile bozulma tespiti
//   • Statik fallback yok — tek, tutarlı yol

#include "heap.h"
#include "vmm.h"
#include <stdint.h>
#include <stddef.h>

// -----------------------------------------------------------------------
// Dış bildirimler
// -----------------------------------------------------------------------
extern void serial_print(const char *s);
extern void print_str64 (const char *s, uint8_t color);
extern void println64   (const char *s, uint8_t color);
extern void *memset64   (void *d, int v, size_t n);
extern void *memcpy64   (void *d, const void *s, size_t n);

#define CLR_GREEN   0x0A
#define CLR_YELLOW  0x0E
#define CLR_RED     0x0C
#define CLR_CYAN    0x03
#define CLR_WHITE   0x0F

// -----------------------------------------------------------------------
// Yardımcı
// -----------------------------------------------------------------------
static void u64_to_dec(uint64_t n, char *buf, int sz) {
    if (sz < 2) return;
    if (n == 0) { buf[0]='0'; buf[1]='\0'; return; }
    int i = 0; char tmp[24];
    while (n && i < (int)sizeof(tmp)-1) { tmp[i++]='0'+(n%10); n/=10; }
    int o=0; while(i-- && o<sz-1) buf[o++]=tmp[i]; buf[o]='\0';
}

// -----------------------------------------------------------------------
// Blok yapısı
//
// Bellek düzeni her blok için:
//   [block_hdr_t | kullanıcı verisi | block_footer_t]
//
// Sınır etiketleme sayesinde önceki bloğun serbest olup olmadığı
// altbilgi üzerinden O(1) kontrol edilir.
// -----------------------------------------------------------------------
#define MAGIC_FREE  0xFEEFCAFEU
#define MAGIC_ALLOC 0xA110CA7EU
#define ALIGN       16U          // minimum hizalama (SSE uyumlu)

typedef struct block_hdr {
    uint32_t          magic;
    uint32_t          flags;        // bit0: is_free
    uint64_t          size;         // yalnızca kullanıcı verisi (hdr + ftr hariç)
    struct block_hdr *prev_blk;     // listede önceki blok
    struct block_hdr *next_blk;     // listede sonraki blok
} block_hdr_t;

typedef struct {
    uint64_t size;                  // == hdr->size (hızlı geriye doğru arama)
    uint32_t magic;
} block_ftr_t;

#define HDR_SIZE  sizeof(block_hdr_t)
#define FTR_SIZE  sizeof(block_ftr_t)
#define OVERHEAD  (HDR_SIZE + FTR_SIZE)

// Bloktan kullanıcı alanı işaretçisi
static inline void *hdr_to_user(block_hdr_t *h) { return (uint8_t *)h + HDR_SIZE; }
// Kullanıcı alanından blok başlığı
static inline block_hdr_t *user_to_hdr(void *p) { return (block_hdr_t *)((uint8_t *)p - HDR_SIZE); }
// Blok başlığından altbilgi
static inline block_ftr_t *hdr_to_ftr(block_hdr_t *h) {
    return (block_ftr_t *)((uint8_t *)h + HDR_SIZE + h->size);
}
// Altbilgiden önceki bloğun başlığı (geriye doğru gezinme)
static inline block_hdr_t *ftr_to_hdr(block_ftr_t *f) {
    return (block_hdr_t *)((uint8_t *)f - sizeof(block_hdr_t) - f->size);
}
// Sonraki bloğun başlığı
static inline block_hdr_t *next_hdr(block_hdr_t *h) {
    return (block_hdr_t *)((uint8_t *)h + HDR_SIZE + h->size + FTR_SIZE);
}

static inline int blk_is_free(block_hdr_t *h) { return h->flags & 1; }

// -----------------------------------------------------------------------
// Heap durumu
// -----------------------------------------------------------------------
static heap_page_alloc_fn s_palloc    = NULL;
static heap_page_free_fn  s_pfree     = NULL;
static int                s_init      = 0;

static uint64_t s_heap_virt = HEAP_VIRT_BASE;  // heap sanal alan başlangıcı
static uint64_t s_heap_end  = HEAP_VIRT_BASE;  // şu anki bitiş (eşlenmiş)
static uint64_t s_heap_max  = HEAP_VIRT_BASE + HEAP_MAX_SIZE;

static block_hdr_t *s_first = NULL;
static block_hdr_t *s_last  = NULL;

static heap_stats_t s_stats;

// -----------------------------------------------------------------------
// Blok başlığı + altbilgisi yaz
// -----------------------------------------------------------------------
static void blk_write(block_hdr_t *h, uint64_t size, int free,
                      block_hdr_t *prev, block_hdr_t *next) {
    h->magic    = free ? MAGIC_FREE : MAGIC_ALLOC;
    h->flags    = free ? 1 : 0;
    h->size     = size;
    h->prev_blk = prev;
    h->next_blk = next;

    block_ftr_t *f = hdr_to_ftr(h);
    f->size  = size;
    f->magic = h->magic;
}

// -----------------------------------------------------------------------
// Bozulma kontrolü
// -----------------------------------------------------------------------
static int blk_valid(block_hdr_t *h) {
    if (!h) return 0;
    if (h->magic != MAGIC_FREE && h->magic != MAGIC_ALLOC) return 0;
    block_ftr_t *f = hdr_to_ftr(h);
    return (f->magic == h->magic) && (f->size == h->size);
}

// -----------------------------------------------------------------------
// Geriye dönük uyumluluk: heap_start / heap_current
// -----------------------------------------------------------------------
uint8_t *heap_start   = (uint8_t *)HEAP_VIRT_BASE;
uint8_t *heap_current = (uint8_t *)HEAP_VIRT_BASE;

static void _heap_update_compat_ptrs(void) {
    heap_start   = (uint8_t *)s_heap_virt;
    heap_current = (uint8_t *)s_heap_end;
}

// -----------------------------------------------------------------------
// brk / sbrk desteği — syscall.c'nin beklediği semboller
// -----------------------------------------------------------------------
uint64_t kmalloc_get_brk(void) {
    return s_heap_end;
}

uint64_t kmalloc_set_brk(uint64_t new_brk) {
    if (!s_init) return (uint64_t)-1;
    if (new_brk <= s_heap_end) return s_heap_end;
    if (new_brk > s_heap_max)  return (uint64_t)-1;

    uint64_t cur = s_heap_end;
    while (cur < new_brk) {
        uint64_t phys = s_palloc(1);
        if (!phys) return (uint64_t)-1;
        if (vmm_map_page(cur, phys, VMM_KERNEL_RW) != 0) return (uint64_t)-1;
        cur += PAGE_SIZE_4K;
        s_stats.heap_pages++;
    }
    s_heap_end    = cur;
    heap_current  = (uint8_t *)s_heap_end;
    return s_heap_end;
}

// -----------------------------------------------------------------------
// Heap genişletme: en az `needed` bayt kadar yeni alan eşle
// -----------------------------------------------------------------------
static int heap_grow(uint64_t needed) {
    uint64_t pages = HEAP_GROW_PAGES;
    uint64_t bytes = pages * PAGE_SIZE_4K;
    if (bytes < needed + OVERHEAD)
        bytes = PAGE_ALIGN_UP(needed + OVERHEAD);

    if (s_heap_end + bytes > s_heap_max) {
        serial_print("[HEAP] HATA: Maksimum heap boyutuna ulasildi!\n");
        return 0;
    }

    // PMM callback ile fiziksel sayfalar al ve VMM ile eşle
    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE_4K) {
        uint64_t phys = s_palloc(1);
        if (!phys) {
            serial_print("[HEAP] HATA: PMM sayfa veremedi!\n");
            return 0;
        }
        if (vmm_map_page(s_heap_end + off, phys, VMM_KERNEL_RW) != 0) {
            serial_print("[HEAP] HATA: VMM esleme basarisiz!\n");
            return 0;
        }
    }

    uint64_t old_end = s_heap_end;
    s_heap_end += bytes;

    // Yeni alanı tek büyük serbest blok olarak oluştur
    uint64_t usable = bytes - OVERHEAD;
    block_hdr_t *nb = (block_hdr_t *)old_end;
    blk_write(nb, usable, 1, s_last, NULL);

    if (s_last) {
        s_last->next_blk = nb;
        // Son blok serbest ise birleştir (isteğe bağlı optimizasyon)
        if (blk_is_free(s_last)) {
            s_last->size     += OVERHEAD + usable;
            hdr_to_ftr(s_last)->size  = s_last->size;
            hdr_to_ftr(s_last)->magic = MAGIC_FREE;
            s_last->next_blk = NULL;
            s_stats.coalesces++;
            // nb artık geçersiz; birleştirme sonrası s_last aynı kalır
            goto done;
        }
    } else {
        s_first = nb;
    }
    s_last = nb;

done:
    s_stats.heap_pages += pages;
    _heap_update_compat_ptrs();
    return 1;
}

// -----------------------------------------------------------------------
// heap_init
// -----------------------------------------------------------------------
void heap_init(heap_page_alloc_fn palloc, heap_page_free_fn pfree) {
    if (s_init) return;

    s_palloc = palloc;
    s_pfree  = pfree;
    s_first  = NULL;
    s_last   = NULL;
    s_heap_virt = HEAP_VIRT_BASE;
    s_heap_end  = HEAP_VIRT_BASE;
    memset64(&s_stats, 0, sizeof(s_stats));

    // İlk sayfa grubunu al
    if (!heap_grow(0)) {
        serial_print("[HEAP] KRITIK: Ilk genisleme basarisiz!\n");
        return;
    }

    s_init = 1;
    serial_print("[HEAP] Baslatildi\n");
}

// -----------------------------------------------------------------------
// kmalloc
// -----------------------------------------------------------------------
void *kmalloc(size_t size) {
    if (!s_init || size == 0) return NULL;

    // Hizalamayı zorla
    size = (size + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);

    // First-fit arama
    block_hdr_t *cur = s_first;
    while (cur) {
        if (!blk_valid(cur)) {
            serial_print("[HEAP] KRITIK: Blok bozulmasi tespit edildi (kmalloc)!\n");
            return NULL;
        }
        if (blk_is_free(cur) && cur->size >= size) break;
        cur = cur->next_blk;
    }

    // Bulunamadı — heap'i genişlet
    if (!cur) {
        if (!heap_grow(size)) return NULL;
        // Son blok artık yeni serbest alan
        cur = s_last;
        if (!cur || !blk_is_free(cur) || cur->size < size) return NULL;
    }

    // Bölme: artık HEAP_MIN_SPLIT + OVERHEAD'den büyükse yeni blok oluştur
    if (cur->size >= size + OVERHEAD + HEAP_MIN_SPLIT) {
        uint64_t remaining = cur->size - size - OVERHEAD;
        block_hdr_t *new_free = (block_hdr_t *)((uint8_t *)cur + HDR_SIZE + size + FTR_SIZE);
        blk_write(new_free, remaining, 1, cur, cur->next_blk);
        if (cur->next_blk) cur->next_blk->prev_blk = new_free;
        else s_last = new_free;
        cur->next_blk = new_free;
        cur->size = size;
        hdr_to_ftr(cur)->size = size;
        s_stats.splits++;
    }

    blk_write(cur, cur->size, 0, cur->prev_blk, cur->next_blk);

    s_stats.total_allocs++;
    s_stats.live_allocs++;
    s_stats.bytes_live += cur->size;
    if (s_stats.bytes_live > s_stats.bytes_peak)
        s_stats.bytes_peak = s_stats.bytes_live;

    return hdr_to_user(cur);
}

// -----------------------------------------------------------------------
// kfree
// -----------------------------------------------------------------------
void kfree(void *ptr) {
    if (!ptr) return;

    block_hdr_t *h = user_to_hdr(ptr);
    if (!blk_valid(h) || blk_is_free(h)) {
        serial_print("[HEAP] UYARI: Gecersiz ya da cift kfree!\n");
        return;
    }

    s_stats.total_frees++;
    s_stats.live_allocs--;
    s_stats.bytes_live -= h->size;

    // Serbest olarak işaretle
    blk_write(h, h->size, 1, h->prev_blk, h->next_blk);

    // Sonraki blok serbest ise birleştir
    if (h->next_blk && blk_is_free(h->next_blk)) {
        block_hdr_t *nx = h->next_blk;
        h->size     += OVERHEAD + nx->size;
        h->next_blk  = nx->next_blk;
        if (nx->next_blk) nx->next_blk->prev_blk = h;
        else s_last = h;
        hdr_to_ftr(h)->size  = h->size;
        hdr_to_ftr(h)->magic = MAGIC_FREE;
        s_stats.coalesces++;
    }

    // Önceki blok serbest ise birleştir
    if (h->prev_blk && blk_is_free(h->prev_blk)) {
        block_hdr_t *pv = h->prev_blk;
        pv->size     += OVERHEAD + h->size;
        pv->next_blk  = h->next_blk;
        if (h->next_blk) h->next_blk->prev_blk = pv;
        else s_last = pv;
        hdr_to_ftr(pv)->size  = pv->size;
        hdr_to_ftr(pv)->magic = MAGIC_FREE;
        s_stats.coalesces++;
    }
}

// -----------------------------------------------------------------------
// krealloc
// -----------------------------------------------------------------------
void *krealloc(void *ptr, size_t new_size) {
    if (!ptr)         return kmalloc(new_size);
    if (new_size == 0){ kfree(ptr); return NULL; }

    block_hdr_t *h = user_to_hdr(ptr);
    if (!blk_valid(h)) return NULL;

    // Yerinde büyütme dene: sonraki blok serbest ve yeterince büyük mü?
    size_t aligned = (new_size + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);
    if (aligned <= h->size) return ptr;  // küçültme — yerinde bırak

    if (h->next_blk && blk_is_free(h->next_blk) &&
        h->size + OVERHEAD + h->next_blk->size >= aligned) {
        block_hdr_t *nx   = h->next_blk;
        uint64_t new_total = h->size + OVERHEAD + nx->size;
        h->next_blk  = nx->next_blk;
        if (nx->next_blk) nx->next_blk->prev_blk = h;
        else s_last = h;
        h->size = new_total;
        blk_write(h, h->size, 0, h->prev_blk, h->next_blk);
        s_stats.bytes_live += (h->size - aligned);
        if (s_stats.bytes_live > s_stats.bytes_peak)
            s_stats.bytes_peak = s_stats.bytes_live;
        return ptr;
    }

    // Yerinde büyütemedi — yeni alan al, kopyala, eskiyi serbest bırak
    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;
    uint64_t copy_size = h->size < aligned ? h->size : aligned;
    memcpy64(new_ptr, ptr, (size_t)copy_size);
    kfree(ptr);
    return new_ptr;
}

// -----------------------------------------------------------------------
// kcalloc
// -----------------------------------------------------------------------
void *kcalloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = kmalloc(total);
    if (p) memset64(p, 0, total);
    return p;
}

// -----------------------------------------------------------------------
// Geriye dönük uyumluluk — init_memory_unified sadece heap_init'i sarmalar
// -----------------------------------------------------------------------

// Kernel, pmm_alloc_frame üzerinden tek frame almak için bu callback'i kullanır.
// heap.c doğrudan pmm.h bağlamaz; kernel64.c bu bridge'i kurar.
// Ancak kolaylık olsun diye burada zayıf bağlantı (weak) alternatifi:
// kernel64.c içinde heap_init(my_palloc, my_pfree) çağrısı yapılmalıdır.

extern uint64_t pmm_alloc_frame(void);   // pmm.h'dan
extern void     pmm_free_frame(uint64_t phys);

static uint64_t default_palloc(uint64_t count) {
    // heap_init'e verilen varsayılan allocator: tek frame
    if (count != 1) return 0;
    return pmm_alloc_frame();
}
static void default_pfree(uint64_t phys, uint64_t count) {
    for (uint64_t i = 0; i < count; i++)
        pmm_free_frame(phys + i * PAGE_SIZE_4K);
}

void init_memory_unified(void) { heap_init(default_palloc, default_pfree); }
void init_memory64(void)       { heap_init(default_palloc, default_pfree); }
void init_memory_gui(void)     { heap_init(default_palloc, default_pfree); }

// -----------------------------------------------------------------------
// İstatistikler
// -----------------------------------------------------------------------
heap_stats_t heap_get_stats(void) { return s_stats; }

void heap_print_stats(void) {
    char buf[24];
    println64("=== Heap Istatistikleri ===", CLR_CYAN);

    print_str64("  Toplam tahsis    : ", CLR_WHITE);
    u64_to_dec(s_stats.total_allocs, buf, sizeof(buf)); println64(buf, CLR_GREEN);

    print_str64("  Toplam serbest   : ", CLR_WHITE);
    u64_to_dec(s_stats.total_frees,  buf, sizeof(buf)); println64(buf, CLR_YELLOW);

    print_str64("  Aktif tahsis     : ", CLR_WHITE);
    u64_to_dec(s_stats.live_allocs,  buf, sizeof(buf)); println64(buf, CLR_GREEN);

    print_str64("  Kullanilan bayt  : ", CLR_WHITE);
    u64_to_dec(s_stats.bytes_live >> 10, buf, sizeof(buf));
    print_str64(buf, CLR_GREEN); println64(" KB", CLR_GREEN);

    print_str64("  En yuksek kullan : ", CLR_WHITE);
    u64_to_dec(s_stats.bytes_peak >> 10, buf, sizeof(buf));
    print_str64(buf, CLR_YELLOW); println64(" KB", CLR_YELLOW);

    print_str64("  Heap sayfalari   : ", CLR_WHITE);
    u64_to_dec(s_stats.heap_pages,   buf, sizeof(buf)); println64(buf, CLR_GREEN);

    print_str64("  Birlesme sayisi  : ", CLR_WHITE);
    u64_to_dec(s_stats.coalesces,    buf, sizeof(buf)); println64(buf, CLR_GREEN);

    print_str64("  Bolme sayisi     : ", CLR_WHITE);
    u64_to_dec(s_stats.splits,       buf, sizeof(buf)); println64(buf, CLR_GREEN);
}

void show_memory_info(void) { heap_print_stats(); }