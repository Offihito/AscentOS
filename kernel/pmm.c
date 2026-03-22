// pmm.c - Physical Memory Manager (Bitmap-based Frame Allocator)
//
// Orijinal memory_unified.c'den ayrıştırıldı.
// Heap kodu → heap.c
//
// Bağımlılıklar:
//   - vmm64.c  : vmm_map_page, vmm_unmap_page, vmm_get_physical_address
//   - vga      : println64, print_str64

#include <stdint.h>
#include <stddef.h>
#include "pmm.h"

// ---------------------------------------------------------------------------
// Platform I/O (kernel VGA console)
// ---------------------------------------------------------------------------
extern void println64  (const char* str, uint8_t color);
extern void print_str64(const char* str, uint8_t color);

// VMM integration (stack allocation)
extern int      vmm_map_page            (uint64_t virt, uint64_t phys, uint64_t flags);
extern int      vmm_unmap_page          (uint64_t virt);
extern uint64_t vmm_get_physical_address(uint64_t virt);

// ---------------------------------------------------------------------------
// VGA colours
// ---------------------------------------------------------------------------
#define VGA_WHITE  0x0F
#define VGA_GREEN  0x0A
#define VGA_YELLOW 0x0E
#define VGA_RED    0x0C
#define VGA_CYAN   0x03

// ---------------------------------------------------------------------------
// Bitmap configuration
// ---------------------------------------------------------------------------
#define PAGE_SIZE_PMM   4096
#define MAX_MEMORY_SIZE (4ULL * 1024ULL * 1024ULL * 1024ULL)  // 4 GB
#define BITMAP_SIZE     (MAX_MEMORY_SIZE / PAGE_SIZE_PMM / 8ULL)  // 131 072 bytes

// ---------------------------------------------------------------------------
// Kernel reserved range
// ---------------------------------------------------------------------------
#define KERNEL_START 0x100000
#define KERNEL_END   0x500000   // 1 MB start + 4 MB kernel

// ---------------------------------------------------------------------------
// Sanal stack havuzları
//
// KERNEL stack havuzu — kernel VMA alanında (PML4[511], zaten kurulu).
//   GRUB'un lower-half PML4 entry'leri phys+offset → non-canonical adres
//   ürettiğinden kernel stack'ler kernel VMA'da tutulur.
//   pmm_alloc_pages(flags=0x3) → bu havuzu kullanır.
//
// USER stack havuzu — lower-half, user-space erişilebilir.
//   is_valid_user_ptr() max 0x00007FFFFFFFFFFF kabul eder.
//   vmm_init() GRUB PML4[0..255] temizlediğinden artık güvenli.
//   pmm_alloc_pages_flags(flags=0x7) → bu havuzu kullanır.
// ---------------------------------------------------------------------------
#define KSTACK_POOL_BASE  0xFFFFFFFF80800000ULL   // kernel image (~4MB) sonrası
#define KSTACK_POOL_MAX   0xFFFFFFFFC0000000ULL   // ~1GB, hepsi PML4[511]

#define USTACK_POOL_BASE  0x0000000010000000ULL   // 256 MB
#define USTACK_POOL_MAX   0x0000000080000000ULL   // 2 GB

// ---------------------------------------------------------------------------
// Default page flags
// ---------------------------------------------------------------------------
#define PMM_FLAGS_KERNEL 0x3ULL   // PRESENT | WRITE
#define PMM_FLAGS_USER   0x7ULL   // PRESENT | WRITE | USER

// ============================================================================
// Private state
// ============================================================================

static unsigned char  bitmap[BITMAP_SIZE];

static unsigned long  total_frames = 0;
static unsigned long  used_frames  = 0;
static unsigned long  free_frames  = 0;

static uint64_t       total_memory_kb   = 0;
static int            pmm_enabled       = 0;
static uint64_t       kstack_pool_cursor = KSTACK_POOL_BASE;  // kernel stack havuzu
static uint64_t       ustack_pool_cursor = USTACK_POOL_BASE;  // user stack havuzu

// ============================================================================
// Bitmap helpers (inline)
// ============================================================================

static inline void bitmap_set_bit(unsigned long idx) {
    bitmap[idx / 8] |= (1u << (idx % 8));
}

static inline void bitmap_clear_bit(unsigned long idx) {
    bitmap[idx / 8] &= ~(1u << (idx % 8));
}

static inline int bitmap_test_bit(unsigned long idx) {
    return (bitmap[idx / 8] & (1u << (idx % 8))) != 0;
}

static inline unsigned long addr_to_frame(unsigned long addr)  { return addr / PAGE_SIZE_PMM; }
static inline unsigned long frame_to_addr(unsigned long frame) { return frame * PAGE_SIZE_PMM; }

static void clear_bitmap(void) {
    for (unsigned long i = 0; i < BITMAP_SIZE; i++) bitmap[i] = 0x00;
}

static void mark_all_used(void) {
    for (unsigned long i = 0; i < BITMAP_SIZE; i++) bitmap[i] = 0xFF;
}

static void mark_region_free(unsigned long base, unsigned long length) {
    unsigned long sf = addr_to_frame(base);
    unsigned long ef = addr_to_frame(base + length);
    for (unsigned long i = sf; i < ef; i++) {
        if (i < total_frames) { bitmap_clear_bit(i); free_frames++; }
    }
}

static void mark_region_used(unsigned long base, unsigned long length) {
    unsigned long sf = addr_to_frame(base);
    unsigned long ef = addr_to_frame(base + length);
    for (unsigned long i = sf; i < ef; i++) {
        if (i < total_frames && !bitmap_test_bit(i)) {
            bitmap_set_bit(i);
            free_frames--;
        }
    }
}

// ============================================================================
// Internal helper: itoa (no libc)
// ============================================================================

static void print_ul(unsigned long val, uint8_t color) {
    char buf[32];
    int  n = 0;
    if (val == 0) { buf[n++] = '0'; }
    else { while (val > 0) { buf[n++] = '0' + (val % 10); val /= 10; } }
    // reverse
    for (int a = 0, b = n - 1; a < b; a++, b--) {
        char c = buf[a]; buf[a] = buf[b]; buf[b] = c;
    }
    buf[n] = '\0';
    print_str64(buf, color);
}

// ============================================================================
// PMM Public Interface
// ============================================================================

void pmm_init(struct memory_map_entry* mmap, unsigned int mmap_count) {
    total_frames = 0;
    used_frames  = 0;
    free_frames  = 0;

    clear_bitmap();

    // Detect maximum usable address
    unsigned long max_addr = 0;
    for (unsigned int i = 0; i < mmap_count; i++) {
        if (mmap[i].type == 1) {
            unsigned long end = mmap[i].base + mmap[i].length;
            if (end > max_addr) max_addr = end;
        }
    }
    if (max_addr > MAX_MEMORY_SIZE) max_addr = MAX_MEMORY_SIZE;

    total_frames   = addr_to_frame(max_addr);
    total_memory_kb = (max_addr + 1023) / 1024;

    // Start with everything used, then free usable regions
    mark_all_used();
    used_frames = total_frames;
    free_frames = 0;

    for (unsigned int i = 0; i < mmap_count; i++) {
        if (mmap[i].type == 1) {
            unsigned long base   = mmap[i].base;
            unsigned long length = mmap[i].length;
            if (base < MAX_MEMORY_SIZE) {
                if (base + length > MAX_MEMORY_SIZE) length = MAX_MEMORY_SIZE - base;
                mark_region_free(base, length);
            }
        }
    }

    // Reserve low 1 MB, kernel image, and bitmap itself
    mark_region_used(0,            0x100000);
    mark_region_used(KERNEL_START, KERNEL_END - KERNEL_START);
    mark_region_used(KERNEL_END,   BITMAP_SIZE);

    used_frames = total_frames - free_frames;
    pmm_enabled = 1;

    print_str64("PMM Initialized: ", VGA_GREEN);
    print_ul((total_frames * PAGE_SIZE_PMM) / (1024 * 1024), VGA_YELLOW);
    println64(" MB detected", VGA_GREEN);
}

// ============================================================================

void* pmm_alloc_frame(void) {
    if (!pmm_enabled) return NULL;
    for (unsigned long i = 0; i < total_frames; i++) {
        if (!bitmap_test_bit(i)) {
            bitmap_set_bit(i);
            used_frames++;
            free_frames--;
            return (void*)frame_to_addr(i);
        }
    }
    return NULL;
}

void pmm_free_frame(void* frame) {
    if (!pmm_enabled || !frame) return;
    unsigned long idx = addr_to_frame((unsigned long)frame);
    if (idx >= total_frames) return;
    if (bitmap_test_bit(idx)) {
        bitmap_clear_bit(idx);
        used_frames--;
        free_frames++;
    }
}

// ============================================================================
// Multi-page allocation (virtual stack pool)
// ============================================================================

void* pmm_alloc_pages(uint64_t count) {
    return pmm_alloc_pages_flags(count, PMM_FLAGS_KERNEL);
}

// ---------------------------------------------------------------------------
// Explicit VA free list for each pool.
//
// vmm_get_physical_address() returns 0 for BOTH unmapped pages AND pages
// covered by boot-time large-page mappings (because vmm_get_pte returns NULL
// for 2MB/1GB PDE entries).  Any approach that uses vmm_get_physical_address
// to probe "is this VA slot free?" is therefore unreliable.
//
// Correct approach: track freed VA ranges explicitly.  pmm_free_pages()
// records the VA base it unmapped.  pmm_alloc_pages_flags() pops a matching
// entry from this list before advancing the cursor.  The cursor is used only
// for fresh (never-allocated) slots; freed slots are recycled via the list.
// ---------------------------------------------------------------------------
#define VA_FREELIST_CAP 64

typedef struct { uint64_t virt_base; uint64_t count; } va_slot_t;

static va_slot_t kstack_free[VA_FREELIST_CAP];
static va_slot_t ustack_free[VA_FREELIST_CAP];
static int kstack_free_n = 0;
static int ustack_free_n = 0;

static void va_free_push(va_slot_t* list, int* n, uint64_t base, uint64_t cnt) {
    if (*n < VA_FREELIST_CAP) {
        list[*n].virt_base = base;
        list[(*n)++].count = cnt;
        return;
    }
    // Full: evict slot 0, shift down, add at end.
    for (int i = 0; i < VA_FREELIST_CAP - 1; i++) list[i] = list[i+1];
    list[VA_FREELIST_CAP-1].virt_base = base;
    list[VA_FREELIST_CAP-1].count     = cnt;
    // *n stays at VA_FREELIST_CAP
}

static int va_free_pop(va_slot_t* list, int* n, uint64_t need, uint64_t* out) {
    for (int i = 0; i < *n; i++) {
        if (list[i].count >= need) {
            *out = list[i].virt_base;
            list[i] = list[--(*n)]; // replace with last entry
            return 1;
        }
    }
    return 0;
}

void* pmm_alloc_pages_flags(uint64_t count, uint64_t map_flags) {
    if (!pmm_enabled || count == 0 || count > 4096) return NULL;
    if (free_frames < count) return NULL;

    int is_user = (map_flags & 0x4) != 0;

    uint64_t pool_base = is_user ? USTACK_POOL_BASE : KSTACK_POOL_BASE;
    uint64_t pool_max  = is_user ? USTACK_POOL_MAX  : KSTACK_POOL_MAX;
    uint64_t* cursor   = is_user ? &ustack_pool_cursor : &kstack_pool_cursor;
    va_slot_t* flist   = is_user ? ustack_free : kstack_free;
    int*       fn      = is_user ? &ustack_free_n : &kstack_free_n;

    // 1. Reuse a freed VA slot if one is available.
    uint64_t virt_base = 0;
    if (!va_free_pop(flist, fn, count, &virt_base)) {
        // 2. Advance cursor for a fresh slot.
        virt_base = *cursor;
        if (virt_base + count * PAGE_SIZE_PMM > pool_max) {
            // Wrap cursor — only valid if pool has room from the start.
            virt_base = pool_base;
        }
        if (virt_base + count * PAGE_SIZE_PMM > pool_max) return NULL;
        *cursor = virt_base + count * PAGE_SIZE_PMM;
    }

    for (uint64_t i = 0; i < count; i++) {
        void* phys = pmm_alloc_frame();
        if (!phys) {
            for (uint64_t j = 0; j < i; j++) {
                uint64_t vp = virt_base + j * PAGE_SIZE_PMM;
                uint64_t pp = vmm_get_physical_address(vp);
                vmm_unmap_page(vp);
                if (pp) pmm_free_frame((void*)pp);
            }
            va_free_push(flist, fn, virt_base, count);
            return NULL;
        }
        uint64_t vp = virt_base + i * PAGE_SIZE_PMM;
        vmm_map_page(vp, (uint64_t)phys, map_flags);
        unsigned char* p = (unsigned char*)vp;
        for (uint64_t b = 0; b < PAGE_SIZE_PMM; b++) p[b] = 0;
    }

    return (void*)virt_base;
}

void pmm_free_pages(void* base, uint64_t count) {
    if (!pmm_enabled || !base || count == 0) return;
    uint64_t virt = (uint64_t)base;

    // Determine which pool this VA belongs to and push to that free list.
    int is_user = (virt >= USTACK_POOL_BASE && virt < USTACK_POOL_MAX);
    int is_kern = (virt >= KSTACK_POOL_BASE && virt < KSTACK_POOL_MAX);

    for (uint64_t j = 0; j < count; j++) {
        uint64_t vp = virt + j * PAGE_SIZE_PMM;
        uint64_t pp = vmm_get_physical_address(vp);
        if (pp) {
            vmm_unmap_page(vp);
            pmm_free_frame((void*)pp);
        }
    }

    // Record the VA range so pmm_alloc_pages_flags can reuse it.
    if (is_user)
        va_free_push(ustack_free, &ustack_free_n, virt, count);
    else if (is_kern)
        va_free_push(kstack_free, &kstack_free_n, virt, count);
}

// ============================================================================
// Statistics
// ============================================================================

unsigned long pmm_get_total_memory(void) { return total_frames * PAGE_SIZE_PMM; }
unsigned long pmm_get_free_memory (void) { return free_frames  * PAGE_SIZE_PMM; }
unsigned long pmm_get_used_memory (void) { return used_frames  * PAGE_SIZE_PMM; }
uint64_t      pmm_get_total_memory_kb(void) { return total_memory_kb; }
int           pmm_is_enabled(void)       { return pmm_enabled; }

void pmm_print_stats(void) {
    if (!pmm_enabled) { println64("PMM not initialized", VGA_RED); return; }
    println64("\nPhysical Memory Manager Statistics:", VGA_CYAN);
    print_str64("  Total Frames: ", VGA_WHITE); print_ul(total_frames, VGA_GREEN); println64("", VGA_GREEN);
    print_str64("  Used Frames:  ", VGA_WHITE); print_ul(used_frames,  VGA_GREEN); println64("", VGA_GREEN);
    print_str64("  Free Frames:  ", VGA_WHITE); print_ul(free_frames,  VGA_GREEN); println64("", VGA_GREEN);
}