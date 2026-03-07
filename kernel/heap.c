// heap.c - Enhanced Kernel Heap Allocator
//
// Orijinal memory_unified.c'den ayrıştırıldı.
// PMM kodu → pmm.c
//
// Özellikler:
//   - İlk-uyum (first-fit) dinamik heap
//   - Blok birleştirme (coalescing)
//   - Blok bölme (splitting)
//   - Statik heap fallback (GUI / erken boot)
//   - sbrk/brk arayüzü (SYS_SBRK syscall)
//   - Çift-serbest bırakma & yığın bozulması tespiti

#include <stdint.h>
#include <stddef.h>
#include "heap.h"
#include "pmm.h"    // pmm_get_total_memory_kb()

// ---------------------------------------------------------------------------
// Platform I/O
// ---------------------------------------------------------------------------
extern void println64  (const char* str, uint8_t color);
extern void print_str64(const char* str, uint8_t color);

// ---------------------------------------------------------------------------
// VGA colours
// ---------------------------------------------------------------------------
#define VGA_WHITE  0x0F
#define VGA_GREEN  0x0A
#define VGA_YELLOW 0x0E
#define VGA_RED    0x0C
#define VGA_CYAN   0x03

// ============================================================================
// Block header
// ============================================================================

#define HEAP_MAGIC      0xDEADBEEF
#define HEAP_FREE_MAGIC 0xFEEEBEEF

typedef struct memory_block {
    uint32_t magic;
    uint64_t size;                  // usable bytes (excluding header)
    int      is_free;
    struct memory_block* next;
    struct memory_block* prev;
} __attribute__((packed)) memory_block_t;

// ============================================================================
// Static fallback heap
// ============================================================================

static uint8_t   static_heap[INITIAL_HEAP_SIZE] __attribute__((aligned(16)));
static uint64_t  static_heap_offset = 0;

// ============================================================================
// Dynamic heap state
// ============================================================================

uint8_t* heap_start   = (uint8_t*)HEAP_START;
uint8_t* heap_end     = (uint8_t*)HEAP_START;
uint8_t* heap_current = (uint8_t*)HEAP_START;

static memory_block_t* first_block = NULL;
static memory_block_t* last_block  = NULL;

// ============================================================================
// Heap statistics
// ============================================================================

static struct {
    uint64_t total_allocations;
    uint64_t total_frees;
    uint64_t current_allocations;
    uint64_t bytes_allocated;
    uint64_t bytes_freed;
    uint64_t peak_usage;
    uint64_t heap_expansions;
    uint64_t coalesces;
    uint64_t block_splits;
} heap_stats = {0};

// ============================================================================
// Mode flag
// ============================================================================

static int use_static_heap = 0;

// ============================================================================
// Internal helper: itoa (no libc)
// ============================================================================

static void print_u64(uint64_t val, uint8_t color) {
    char buf[32];
    int  n = 0;
    if (val == 0) { buf[n++] = '0'; }
    else { while (val > 0) { buf[n++] = '0' + (val % 10); val /= 10; } }
    for (int a = 0, b = n - 1; a < b; a++, b--) {
        char c = buf[a]; buf[a] = buf[b]; buf[b] = c;
    }
    buf[n] = '\0';
    print_str64(buf, color);
}

// ============================================================================
// Block helpers
// ============================================================================

static int validate_block(memory_block_t* b) {
    if (!b) return 0;
    return (b->magic == HEAP_MAGIC ||
            (b->is_free && b->magic == HEAP_FREE_MAGIC));
}

static void coalesce_next(memory_block_t* b) {
    if (!b || !b->next) return;
    if (!b->is_free || !b->next->is_free) return;

    memory_block_t* nx = b->next;
    b->size += sizeof(memory_block_t) + nx->size;
    b->next  = nx->next;
    if (nx->next) nx->next->prev = b;
    else          last_block = b;
    heap_stats.coalesces++;
}

static void coalesce_prev(memory_block_t* b) {
    if (!b || !b->prev) return;
    if (!b->is_free || !b->prev->is_free) return;
    coalesce_next(b->prev);
}

static int expand_heap(uint64_t min_size) {
    if (use_static_heap) return 0;

    uint64_t expand_size = HEAP_EXPAND_SIZE;
    if (min_size > expand_size)
        expand_size = ((min_size + HEAP_EXPAND_SIZE - 1) / HEAP_EXPAND_SIZE) * HEAP_EXPAND_SIZE;

    uint64_t current_size = (uint64_t)(heap_current - heap_start);
    if (current_size + expand_size > MAX_HEAP_SIZE) return 0;

    // In a full system this would call pmm_alloc_frame + vmm_map_page
    heap_end = heap_current + expand_size;
    heap_stats.heap_expansions++;
    return 1;
}

static void split_block(memory_block_t* b, uint64_t size) {
    uint64_t remainder = b->size - size;
    if (remainder <= sizeof(memory_block_t) + 32) return;  // not worth it

    memory_block_t* nb = (memory_block_t*)((uint8_t*)b + sizeof(memory_block_t) + size);
    nb->magic   = HEAP_FREE_MAGIC;
    nb->size    = remainder - sizeof(memory_block_t);
    nb->is_free = 1;
    nb->next    = b->next;
    nb->prev    = b;

    if (b->next) b->next->prev = nb;
    else         last_block = nb;

    b->size = size;
    b->next = nb;
    heap_stats.block_splits++;
}

// ============================================================================
// Initialization
// ============================================================================

void init_heap(void) {
    heap_start   = (uint8_t*)HEAP_START;
    heap_current = (uint8_t*)HEAP_START;
    heap_end     = (uint8_t*)(HEAP_START + INITIAL_HEAP_SIZE);
    first_block  = NULL;
    last_block   = NULL;

    heap_stats.total_allocations  = 0;
    heap_stats.total_frees        = 0;
    heap_stats.current_allocations = 0;
    heap_stats.bytes_allocated    = 0;
    heap_stats.bytes_freed        = 0;
    heap_stats.peak_usage         = 0;
    heap_stats.heap_expansions    = 0;
    heap_stats.coalesces          = 0;
    heap_stats.block_splits       = 0;

    print_str64("  OK Heap initialized: ", VGA_GREEN);
    print_u64(INITIAL_HEAP_SIZE / (1024 * 1024), VGA_GREEN);
    println64(" MB", VGA_GREEN);
}

// Backward-compat aliases
void init_memory_unified(void) { init_heap(); }
void init_memory64      (void) { init_heap(); }
void init_memory_gui    (void) { init_heap(); }

// ============================================================================
// kmalloc
// ============================================================================

void* kmalloc(size_t size) {
    if (size == 0) return NULL;

    // --- Static bump allocator ---
    if (use_static_heap) {
        size = (size + 15) & ~15u;
        if (static_heap_offset + size > INITIAL_HEAP_SIZE) {
            print_str64("ERROR: Out of static heap memory\n", VGA_RED);
            return NULL;
        }
        void* ptr = &static_heap[static_heap_offset];
        static_heap_offset += size;
        return ptr;
    }

    // --- Dynamic heap: align to 8 bytes ---
    size = (size + 7) & ~7u;

    // First ever allocation
    if (first_block == NULL) {
        if (sizeof(memory_block_t) + size > (uint64_t)(heap_end - heap_current)) {
            if (!expand_heap(sizeof(memory_block_t) + size)) {
                print_str64("ERROR: Out of heap memory\n", VGA_RED);
                return NULL;
            }
        }
        first_block          = (memory_block_t*)heap_current;
        first_block->magic   = HEAP_MAGIC;
        first_block->size    = size;
        first_block->is_free = 0;
        first_block->next    = NULL;
        first_block->prev    = NULL;
        last_block           = first_block;
        heap_current        += sizeof(memory_block_t) + size;

        heap_stats.total_allocations++;
        heap_stats.current_allocations++;
        heap_stats.bytes_allocated += size;
        if (heap_stats.bytes_allocated > heap_stats.peak_usage)
            heap_stats.peak_usage = heap_stats.bytes_allocated;

        return (uint8_t*)first_block + sizeof(memory_block_t);
    }

    // First-fit search
    for (memory_block_t* b = first_block; b != NULL; b = b->next) {
        if (!validate_block(b)) {
            print_str64("ERROR: Heap corruption detected!\n", VGA_RED);
            return NULL;
        }
        if (b->is_free && b->size >= size) {
            b->is_free = 0;
            b->magic   = HEAP_MAGIC;
            split_block(b, size);

            heap_stats.total_allocations++;
            heap_stats.current_allocations++;
            heap_stats.bytes_allocated += size;
            uint64_t live = heap_stats.bytes_allocated - heap_stats.bytes_freed;
            if (live > heap_stats.peak_usage) heap_stats.peak_usage = live;

            return (uint8_t*)b + sizeof(memory_block_t);
        }
    }

    // Allocate new block at end
    uint64_t needed = sizeof(memory_block_t) + size;
    if (needed > (uint64_t)(heap_end - heap_current)) {
        if (!expand_heap(needed)) {
            print_str64("ERROR: Out of heap memory\n", VGA_RED);
            return NULL;
        }
    }

    memory_block_t* nb = (memory_block_t*)heap_current;
    nb->magic          = HEAP_MAGIC;
    nb->size           = size;
    nb->is_free        = 0;
    nb->next           = NULL;
    nb->prev           = last_block;

    if (last_block) last_block->next = nb;
    last_block  = nb;
    heap_current += needed;

    heap_stats.total_allocations++;
    heap_stats.current_allocations++;
    heap_stats.bytes_allocated += size;
    uint64_t live = heap_stats.bytes_allocated - heap_stats.bytes_freed;
    if (live > heap_stats.peak_usage) heap_stats.peak_usage = live;

    return (uint8_t*)nb + sizeof(memory_block_t);
}

// ============================================================================
// kfree
// ============================================================================

void kfree(void* ptr) {
    if (!ptr) return;
    if (use_static_heap) return;  // static heap has no free

    memory_block_t* b = (memory_block_t*)((uint8_t*)ptr - sizeof(memory_block_t));
    if (!validate_block(b)) { print_str64("ERROR: Invalid free - heap corruption!\n", VGA_RED); return; }
    if (b->is_free)         { print_str64("WARNING: Double free detected!\n", VGA_YELLOW);      return; }

    b->is_free = 1;
    b->magic   = HEAP_FREE_MAGIC;

    heap_stats.total_frees++;
    heap_stats.current_allocations--;
    heap_stats.bytes_freed += b->size;

    coalesce_next(b);
    coalesce_prev(b);
}

// ============================================================================
// krealloc
// ============================================================================

void* krealloc(void* ptr, size_t new_size) {
    if (!ptr)      return kmalloc(new_size);
    if (!new_size) { kfree(ptr); return NULL; }

    if (use_static_heap) {
        void* np = kmalloc(new_size);
        if (!np) return NULL;
        uint8_t* s = (uint8_t*)ptr;
        uint8_t* d = (uint8_t*)np;
        for (size_t i = 0; i < new_size; i++) d[i] = s[i];
        return np;
    }

    memory_block_t* b = (memory_block_t*)((uint8_t*)ptr - sizeof(memory_block_t));
    if (!validate_block(b)) return NULL;

    if (new_size <= b->size) return ptr;

    // Try to absorb free next block
    if (b->next && b->next->is_free) {
        uint64_t combined = b->size + sizeof(memory_block_t) + b->next->size;
        if (combined >= new_size) { coalesce_next(b); return ptr; }
    }

    void* np = kmalloc(new_size);
    if (!np) return NULL;
    uint8_t* s = (uint8_t*)ptr;
    uint8_t* d = (uint8_t*)np;
    for (uint64_t i = 0; i < b->size; i++) d[i] = s[i];
    kfree(ptr);
    return np;
}

// ============================================================================
// kcalloc
// ============================================================================

void* kcalloc(size_t num, size_t size) {
    uint64_t total = (uint64_t)num * size;
    void*    ptr   = kmalloc((size_t)total);
    if (ptr) {
        uint8_t* p = (uint8_t*)ptr;
        for (uint64_t i = 0; i < total; i++) p[i] = 0;
    }
    return ptr;
}

// ============================================================================
// GUI aliases
// ============================================================================

void* malloc_gui(uint64_t size) { return kmalloc((size_t)size); }
void  free_gui  (void*    ptr)  { kfree(ptr); }

// ============================================================================
// sbrk / brk support
// ============================================================================

uint64_t kmalloc_get_brk(void) { return (uint64_t)heap_current; }

uint64_t kmalloc_set_brk(uint64_t new_brk) {
    if (new_brk < (uint64_t)heap_current)                return (uint64_t)-1;
    if (new_brk > (uint64_t)heap_start + MAX_HEAP_SIZE)  return (uint64_t)-1;
    if (new_brk > (uint64_t)heap_end) {
        heap_end = (uint8_t*)new_brk;
        heap_stats.heap_expansions++;
    }
    heap_current = (uint8_t*)new_brk;
    return (uint64_t)heap_current;
}

// ============================================================================
// Misc
// ============================================================================

void* map_page(uint64_t physical, uint64_t virtual_addr __attribute__((unused))) {
    return (void*)physical;  // identity mapping stub
}

uint64_t get_total_memory(void) {
    return pmm_get_total_memory_kb() * 1024;
}

void set_static_heap_mode(int enable) {
    use_static_heap = enable;
    println64(enable ? "Switched to static heap mode"
                     : "Switched to dynamic heap mode", VGA_YELLOW);
}

// ============================================================================
// show_memory_info
// ============================================================================

void show_memory_info(void) {
    if (use_static_heap) {
        println64("\n=== Memory Statistics (Static Heap) ===", VGA_CYAN);
        print_str64("  Heap Size: ", VGA_WHITE);
        print_u64(INITIAL_HEAP_SIZE / 1024, VGA_GREEN);
        println64(" KB", VGA_GREEN);

        print_str64("  Used: ", VGA_WHITE);
        print_u64(static_heap_offset / 1024, VGA_GREEN);
        println64(" KB", VGA_GREEN);
        return;
    }

    println64("\n=== Memory Statistics (Dynamic Heap) ===", VGA_CYAN);

    print_str64("  Total Allocations: ", VGA_WHITE);
    print_u64(heap_stats.total_allocations,   VGA_GREEN);  println64("", VGA_GREEN);

    print_str64("  Total Frees:       ", VGA_WHITE);
    print_u64(heap_stats.total_frees,         VGA_YELLOW); println64("", VGA_YELLOW);

    print_str64("  Active Allocs:     ", VGA_WHITE);
    print_u64(heap_stats.current_allocations, VGA_CYAN);   println64("", VGA_CYAN);

    print_str64("  Peak Usage:        ", VGA_WHITE);
    print_u64(heap_stats.peak_usage / 1024,   VGA_GREEN);
    println64(" KB", VGA_GREEN);

    print_str64("  Heap Expansions:   ", VGA_WHITE);
    print_u64(heap_stats.heap_expansions,     VGA_YELLOW); println64("", VGA_YELLOW);

    print_str64("  Block Coalesces:   ", VGA_WHITE);
    print_u64(heap_stats.coalesces,           VGA_CYAN);   println64("", VGA_CYAN);

    // Count blocks
    uint64_t total_blocks = 0, free_blocks = 0;
    for (memory_block_t* b = first_block; b; b = b->next) {
        total_blocks++;
        if (b->is_free) free_blocks++;
    }
    print_str64("  Total Blocks:      ", VGA_WHITE);
    print_u64(total_blocks, VGA_GREEN);
    print_str64(" (", VGA_WHITE);
    print_u64(free_blocks, VGA_YELLOW);
    println64(" free)", VGA_WHITE);
}