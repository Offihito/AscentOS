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
// Virtual stack pool (256 MB – 2 GB)
// ---------------------------------------------------------------------------
#define STACK_POOL_BASE  0x10000000ULL
#define STACK_POOL_MAX   0x80000000ULL

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
static uint64_t       stack_pool_cursor = STACK_POOL_BASE;

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

void* pmm_alloc_pages_flags(uint64_t count, uint64_t map_flags) {
    if (!pmm_enabled || count == 0 || count > 4096) return NULL;
    if (free_frames < count) return NULL;

    uint64_t virt_base = stack_pool_cursor;
    uint64_t virt_end  = virt_base + count * PAGE_SIZE_PMM;

    if (virt_end > STACK_POOL_MAX) {
        // wrap (simple policy – production OS would track allocations)
        stack_pool_cursor = STACK_POOL_BASE;
        virt_base         = stack_pool_cursor;
        virt_end          = virt_base + count * PAGE_SIZE_PMM;
        if (virt_end > STACK_POOL_MAX) return NULL;
    }
    stack_pool_cursor = virt_end;

    for (uint64_t i = 0; i < count; i++) {
        void* phys = pmm_alloc_frame();
        if (!phys) {
            // rollback
            for (uint64_t j = 0; j < i; j++) {
                uint64_t vp = virt_base + j * PAGE_SIZE_PMM;
                uint64_t pp = vmm_get_physical_address(vp);
                vmm_unmap_page(vp);
                if (pp) pmm_free_frame((void*)pp);
            }
            return NULL;
        }

        uint64_t vp = virt_base + i * PAGE_SIZE_PMM;
        vmm_map_page(vp, (uint64_t)phys, map_flags);

        // zero page
        unsigned char* p = (unsigned char*)vp;
        for (uint64_t b = 0; b < PAGE_SIZE_PMM; b++) p[b] = 0;
    }

    return (void*)virt_base;
}

void pmm_free_pages(void* base, uint64_t count) {
    if (!pmm_enabled || !base || count == 0) return;
    uint64_t virt = (uint64_t)base;
    for (uint64_t j = 0; j < count; j++) {
        uint64_t vp = virt + j * PAGE_SIZE_PMM;
        uint64_t pp = vmm_get_physical_address(vp);
        if (pp) {
            vmm_unmap_page(vp);
            pmm_free_frame((void*)pp);
        }
    }
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