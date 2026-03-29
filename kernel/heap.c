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
#include "spinlock64.h"

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
// Spinlocks
// ============================================================================

// Protects: first_block/last_block list, heap_current/heap_end, heap_stats
static spinlock_t heap_lock = SPINLOCK_INIT;

// Per-cache lock embedded in slab_cache_t — declared alongside that struct below.
// Protects: slab linked list, slab bitmaps, per-cache stats.

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
// kmalloc  (internal unlocked version used by krealloc and slab_grow_cache)
// ============================================================================

static void* _kmalloc_unlocked(size_t size) {
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
// kmalloc  (public — IRQ-safe lock wrapper)
// ============================================================================

void* kmalloc(size_t size) {
    uint64_t flags = spinlock_lock_irq(&heap_lock);
    void* ptr = _kmalloc_unlocked(size);
    spinlock_unlock_irq(&heap_lock, flags);
    return ptr;
}

// ============================================================================
// kfree
// ============================================================================

static void _kfree_unlocked(void* ptr) {
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

void kfree(void* ptr) {
    uint64_t flags = spinlock_lock_irq(&heap_lock);
    _kfree_unlocked(ptr);
    spinlock_unlock_irq(&heap_lock, flags);
}

// ============================================================================
// krealloc
// ============================================================================

void* krealloc(void* ptr, size_t new_size) {
    if (!ptr)      return kmalloc(new_size);
    if (!new_size) { kfree(ptr); return NULL; }

    uint64_t flags = spinlock_lock_irq(&heap_lock);

    if (use_static_heap) {
        void* np = _kmalloc_unlocked(new_size);
        if (!np) { spinlock_unlock_irq(&heap_lock, flags); return NULL; }
        uint8_t* s = (uint8_t*)ptr;
        uint8_t* d = (uint8_t*)np;
        for (size_t i = 0; i < new_size; i++) d[i] = s[i];
        spinlock_unlock_irq(&heap_lock, flags);
        return np;
    }

    memory_block_t* b = (memory_block_t*)((uint8_t*)ptr - sizeof(memory_block_t));
    if (!validate_block(b)) { spinlock_unlock_irq(&heap_lock, flags); return NULL; }

    if (new_size <= b->size) { spinlock_unlock_irq(&heap_lock, flags); return ptr; }

    // Try to absorb free next block
    if (b->next && b->next->is_free) {
        uint64_t combined = b->size + sizeof(memory_block_t) + b->next->size;
        if (combined >= new_size) {
            coalesce_next(b);
            spinlock_unlock_irq(&heap_lock, flags);
            return ptr;
        }
    }

    void* np = _kmalloc_unlocked(new_size);
    if (!np) { spinlock_unlock_irq(&heap_lock, flags); return NULL; }
    uint8_t* s = (uint8_t*)ptr;
    uint8_t* d = (uint8_t*)np;
    for (uint64_t i = 0; i < b->size; i++) d[i] = s[i];
    _kfree_unlocked(ptr);
    spinlock_unlock_irq(&heap_lock, flags);
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

// ============================================================================
// Slab Allocator
// ============================================================================
//
// Design:
//   - One slab_cache_t per fixed size class (8, 16, 32 … 1024 bytes).
//   - Each cache grows by allocating new slab_t backing blocks from kmalloc.
//   - A slab holds SLAB_OBJECTS_PER_SLAB objects and a compact bitmap.
//   - Allocation is O(caches * slabs) — perfectly acceptable for a kernel.
//   - Free is O(1) once the owning slab is identified via pointer range.
// ============================================================================

// ------- size-class table -------
static const uint32_t slab_sizes[SLAB_NUM_CACHES] = {
    SLAB_SIZE_8, SLAB_SIZE_16, SLAB_SIZE_32,  SLAB_SIZE_64,
    SLAB_SIZE_128, SLAB_SIZE_256, SLAB_SIZE_512, SLAB_SIZE_1024
};

static slab_cache_t slab_caches[SLAB_NUM_CACHES];
static int          slab_initialized = 0;

// ------- bitmap helpers -------
static inline void bm_set  (uint8_t* bm, uint32_t idx) { bm[idx / 8] |=  (1u << (idx % 8)); }
static inline void bm_clear(uint8_t* bm, uint32_t idx) { bm[idx / 8] &= ~(1u << (idx % 8)); }
static inline int  bm_test (uint8_t* bm, uint32_t idx) { return (bm[idx / 8] >> (idx % 8)) & 1; }

// ------- find the cache index for a requested size -------
static int slab_cache_index(uint32_t size) {
    for (int i = 0; i < SLAB_NUM_CACHES; i++) {
        if (size <= slab_sizes[i]) return i;
    }
    return -1;   // too large — fall back to kmalloc
}

// ------- grow a cache by adding one backing slab -------
static slab_t* slab_grow_cache(slab_cache_t* cache) {
    uint32_t cap  = SLAB_OBJECTS_PER_SLAB;
    uint32_t osiz = cache->obj_size;

    // We need: slab header + bitmap (ceil(cap/8) bytes) + data (cap * osiz bytes)
    uint32_t bm_bytes   = (cap + 7) / 8;
    uint32_t data_bytes = cap * osiz;
    uint32_t total      = (uint32_t)sizeof(slab_t) + bm_bytes + data_bytes;

    slab_t* s = (slab_t*)kmalloc(total);
    if (!s) return NULL;

    // Zero the whole allocation
    uint8_t* raw = (uint8_t*)s;
    for (uint32_t i = 0; i < total; i++) raw[i] = 0;

    s->magic    = SLAB_MAGIC;
    s->obj_size = osiz;
    s->capacity = cap;
    s->used     = 0;
    s->bitmap   = raw + sizeof(slab_t);
    s->data     = raw + sizeof(slab_t) + bm_bytes;
    s->next     = cache->head;

    cache->head       = s;
    cache->slab_count++;
    return s;
}

// ============================================================================
// slab_init
// ============================================================================
void slab_init(void) {
    for (int i = 0; i < SLAB_NUM_CACHES; i++) {
        slab_caches[i].obj_size       = slab_sizes[i];
        slab_caches[i].head           = NULL;
        slab_caches[i].lock           = (spinlock_t)SPINLOCK_INIT;
        slab_caches[i].total_allocs   = 0;
        slab_caches[i].total_frees    = 0;
        slab_caches[i].active_objects = 0;
        slab_caches[i].slab_count     = 0;
    }
    slab_initialized = 1;

    print_str64("  OK Slab allocator initialized: ", VGA_GREEN);
    print_u64(SLAB_NUM_CACHES, VGA_GREEN);
    println64(" size-classes", VGA_GREEN);
}

// ============================================================================
// slab_alloc
// ============================================================================
void* slab_alloc(uint32_t size) {
    if (!slab_initialized || size == 0) return NULL;

    int ci = slab_cache_index(size);
    if (ci < 0) {
        // Larger than the biggest slab class — delegate to kmalloc
        return kmalloc(size);
    }

    slab_cache_t* cache = &slab_caches[ci];
    uint64_t flags = spinlock_lock_irq(&cache->lock);

    // Walk existing slabs looking for a free slot
    for (slab_t* s = cache->head; s; s = s->next) {
        if (s->used >= s->capacity) continue;
        for (uint32_t idx = 0; idx < s->capacity; idx++) {
            if (!bm_test(s->bitmap, idx)) {
                bm_set(s->bitmap, idx);
                s->used++;
                cache->total_allocs++;
                cache->active_objects++;
                spinlock_unlock_irq(&cache->lock, flags);
                return s->data + idx * s->obj_size;
            }
        }
    }

    // All slabs full — must grow. Drop cache lock first to preserve
    // lock ordering (heap_lock is acquired inside kmalloc).
    spinlock_unlock_irq(&cache->lock, flags);
    slab_t* ns = slab_grow_cache(cache);
    if (!ns) return NULL;

    // Re-acquire cache lock to update the new slab's slot
    flags = spinlock_lock_irq(&cache->lock);
    bm_set(ns->bitmap, 0);
    ns->used = 1;
    cache->total_allocs++;
    cache->active_objects++;
    spinlock_unlock_irq(&cache->lock, flags);
    return ns->data;
}

// ============================================================================
// slab_owns  (1 if ptr lives inside any slab's data region)
// ============================================================================
int slab_owns(void* ptr) {
    if (!ptr || !slab_initialized) return 0;
    uint8_t* p = (uint8_t*)ptr;
    for (int ci = 0; ci < SLAB_NUM_CACHES; ci++) {
        slab_cache_t* cache = &slab_caches[ci];
        uint64_t flags = spinlock_lock_irq(&cache->lock);
        for (slab_t* s = cache->head; s; s = s->next) {
            uint8_t* lo = s->data;
            uint8_t* hi = lo + s->capacity * s->obj_size;
            if (p >= lo && p < hi) {
                spinlock_unlock_irq(&cache->lock, flags);
                return 1;
            }
        }
        spinlock_unlock_irq(&cache->lock, flags);
    }
    return 0;
}

// ============================================================================
// slab_free
// ============================================================================
void slab_free(void* ptr) {
    if (!ptr || !slab_initialized) return;
    uint8_t* p = (uint8_t*)ptr;

    for (int ci = 0; ci < SLAB_NUM_CACHES; ci++) {
        slab_cache_t* cache = &slab_caches[ci];
        uint64_t flags = spinlock_lock_irq(&cache->lock);
        for (slab_t* s = cache->head; s; s = s->next) {
            uint8_t* lo = s->data;
            uint8_t* hi = lo + s->capacity * s->obj_size;
            if (p < lo || p >= hi) continue;

            // Pointer belongs to this slab — find slot index
            uint32_t off = (uint32_t)(p - lo);
            if (off % s->obj_size != 0) {
                spinlock_unlock_irq(&cache->lock, flags);
                print_str64("SLAB WARNING: unaligned slab_free!\n", VGA_YELLOW);
                return;
            }
            uint32_t idx = off / s->obj_size;
            if (!bm_test(s->bitmap, idx)) {
                spinlock_unlock_irq(&cache->lock, flags);
                print_str64("SLAB WARNING: double free detected!\n", VGA_YELLOW);
                return;
            }
            bm_clear(s->bitmap, idx);
            s->used--;
            cache->total_frees++;
            cache->active_objects--;
            spinlock_unlock_irq(&cache->lock, flags);
            return;
        }
        spinlock_unlock_irq(&cache->lock, flags);
    }
    // Not found in any slab — try the heap free
    kfree(ptr);
}

// ============================================================================
// slab_stats  (pretty print to VGA)
// ============================================================================
void slab_stats(void) {
    if (!slab_initialized) {
        println64("Slab allocator not initialized.", VGA_RED);
        return;
    }
    println64("\n=== Slab Allocator Statistics ===", VGA_CYAN);
    for (int ci = 0; ci < SLAB_NUM_CACHES; ci++) {
        slab_cache_t* c = &slab_caches[ci];

        print_str64("  [", VGA_WHITE);
        // pad size to 4 chars
        if (c->obj_size < 1000) print_str64(" ", VGA_WHITE);
        if (c->obj_size < 100)  print_str64(" ", VGA_WHITE);
        if (c->obj_size < 10)   print_str64(" ", VGA_WHITE);
        print_u64(c->obj_size, VGA_YELLOW);
        print_str64("B] slabs=", VGA_WHITE);
        print_u64(c->slab_count, VGA_GREEN);
        print_str64("  active=", VGA_WHITE);
        print_u64(c->active_objects, VGA_GREEN);
        print_str64("  allocs=", VGA_WHITE);
        print_u64(c->total_allocs, VGA_CYAN);
        print_str64("  frees=", VGA_WHITE);
        print_u64(c->total_frees, VGA_CYAN);
        println64("", VGA_WHITE);
    }
}

// ============================================================================
// slab_query_cache  — thin accessor so command handlers can read stats
//                     without needing slab_cache_t to be public
// ============================================================================
void slab_query_cache(int index,
                      uint32_t* out_obj_size,
                      uint64_t* out_slabs,
                      uint64_t* out_active,
                      uint64_t* out_allocs,
                      uint64_t* out_frees) {
    if (index < 0 || index >= SLAB_NUM_CACHES || !slab_initialized) {
        if (out_obj_size) *out_obj_size = 0;
        if (out_slabs)    *out_slabs    = 0;
        if (out_active)   *out_active   = 0;
        if (out_allocs)   *out_allocs   = 0;
        if (out_frees)    *out_frees    = 0;
        return;
    }
    slab_cache_t* c = &slab_caches[index];
    uint64_t flags = spinlock_lock_irq(&c->lock);
    if (out_obj_size) *out_obj_size = c->obj_size;
    if (out_slabs)    *out_slabs    = c->slab_count;
    if (out_active)   *out_active   = c->active_objects;
    if (out_allocs)   *out_allocs   = c->total_allocs;
    if (out_frees)    *out_frees    = c->total_frees;
    spinlock_unlock_irq(&c->lock, flags);
}

// ============================================================================
// slab_stats_output  — writes per-cache stats into a CommandOutput buffer
//   output is typed void* to avoid pulling commands64.h into heap.c;
//   we replicate the tiny output_add_line ABI inline.
// ============================================================================

// CommandOutput ABI (must match commands64.h)
#define _SO_MAX_LINES  128
#define _SO_MAX_LEN    256

typedef struct {
    char    lines[_SO_MAX_LINES][_SO_MAX_LEN];
    uint8_t colors[_SO_MAX_LINES];
    int     line_count;
} _slab_output_t;

static void _so_add(void* out, const char* s, uint8_t color) {
    _slab_output_t* o = (_slab_output_t*)out;
    if (!o || o->line_count >= _SO_MAX_LINES) return;
    int i = 0;
    while (s[i] && i < _SO_MAX_LEN - 1) {
        o->lines[o->line_count][i] = s[i];
        i++;
    }
    o->lines[o->line_count][i] = '\0';
    o->colors[o->line_count]   = color;
    o->line_count++;
}

static void _so_u64(char* buf, uint64_t v) {
    int n = 0;
    if (v == 0) { buf[n++] = '0'; }
    else { while (v) { buf[n++] = '0' + (v % 10); v /= 10; } }
    for (int a = 0, b = n - 1; a < b; a++, b--) {
        char c = buf[a]; buf[a] = buf[b]; buf[b] = c;
    }
    buf[n] = '\0';
}

static void _so_concat(char* dst, const char* src) {
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

void slab_stats_output(void* output) {
    if (!output) { slab_stats(); return; }

    _so_add(output, "Per-cache statistics:", VGA_CYAN);
    _so_add(output, "  [ Size]  Slabs  Active  Allocs   Frees", VGA_WHITE);

    if (!slab_initialized) {
        _so_add(output, "  (not initialized)", VGA_RED);
        return;
    }

    char tmp[16];
    for (int ci = 0; ci < SLAB_NUM_CACHES; ci++) {
        slab_cache_t* c = &slab_caches[ci];
        char row[80];

        row[0] = ' '; row[1] = ' '; row[2] = '['; row[3] = '\0';
        if (c->obj_size < 1000) _so_concat(row, " ");
        if (c->obj_size < 100)  _so_concat(row, " ");
        if (c->obj_size < 10)   _so_concat(row, " ");
        _so_u64(tmp, c->obj_size); _so_concat(row, tmp);
        _so_concat(row, "B]  ");

        _so_u64(tmp, c->slab_count);    _so_concat(row, tmp); _so_concat(row, "      ");
        _so_u64(tmp, c->active_objects); _so_concat(row, tmp); _so_concat(row, "      ");
        _so_u64(tmp, c->total_allocs);  _so_concat(row, tmp); _so_concat(row, "      ");
        _so_u64(tmp, c->total_frees);   _so_concat(row, tmp);

        uint8_t color = (c->active_objects > 0) ? VGA_YELLOW : VGA_GREEN;
        _so_add(output, row, color);
    }
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

    // Take a snapshot under lock, then print without the lock held
    uint64_t snap_total_alloc, snap_total_frees, snap_current_alloc;
    uint64_t snap_peak, snap_expansions, snap_coalesces;
    uint64_t total_blocks = 0, free_blocks = 0;

    uint64_t flags = spinlock_lock_irq(&heap_lock);
    snap_total_alloc   = heap_stats.total_allocations;
    snap_total_frees   = heap_stats.total_frees;
    snap_current_alloc = heap_stats.current_allocations;
    snap_peak          = heap_stats.peak_usage;
    snap_expansions    = heap_stats.heap_expansions;
    snap_coalesces     = heap_stats.coalesces;
    for (memory_block_t* b = first_block; b; b = b->next) {
        total_blocks++;
        if (b->is_free) free_blocks++;
    }
    spinlock_unlock_irq(&heap_lock, flags);

    println64("\n=== Memory Statistics (Dynamic Heap) ===", VGA_CYAN);

    print_str64("  Total Allocations: ", VGA_WHITE);
    print_u64(snap_total_alloc,   VGA_GREEN);  println64("", VGA_GREEN);

    print_str64("  Total Frees:       ", VGA_WHITE);
    print_u64(snap_total_frees,   VGA_YELLOW); println64("", VGA_YELLOW);

    print_str64("  Active Allocs:     ", VGA_WHITE);
    print_u64(snap_current_alloc, VGA_CYAN);   println64("", VGA_CYAN);

    print_str64("  Peak Usage:        ", VGA_WHITE);
    print_u64(snap_peak / 1024,   VGA_GREEN);
    println64(" KB", VGA_GREEN);

    print_str64("  Heap Expansions:   ", VGA_WHITE);
    print_u64(snap_expansions,    VGA_YELLOW); println64("", VGA_YELLOW);

    print_str64("  Block Coalesces:   ", VGA_WHITE);
    print_u64(snap_coalesces,     VGA_CYAN);   println64("", VGA_CYAN);

    print_str64("  Total Blocks:      ", VGA_WHITE);
    print_u64(total_blocks, VGA_GREEN);
    print_str64(" (", VGA_WHITE);
    print_u64(free_blocks, VGA_YELLOW);
    println64(" free)", VGA_WHITE);
}