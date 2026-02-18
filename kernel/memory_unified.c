// memory_unified.c - Enhanced Unified Memory Management with Advanced Heap
// Improved malloc/free with block coalescing and dynamic heap expansion

#include <stdint.h>
#include <stddef.h>
#include "memory_unified.h"

extern void println64(const char* str, uint8_t color);
extern void print_str64(const char* str, uint8_t color);

#define VGA_WHITE 0x0F
#define VGA_GREEN 0x0A
#define VGA_YELLOW 0x0E
#define VGA_RED 0x0C
#define VGA_CYAN 0x03

// Page size
#define PAGE_SIZE 4096

// Heap configuration
#define INITIAL_HEAP_SIZE (4 * 1024 * 1024)  // 4MB initial heap
#define MAX_HEAP_SIZE (64 * 1024 * 1024)     // 64MB max heap
#define HEAP_START 0x200000                   // 2MB start
#define HEAP_EXPAND_SIZE (1 * 1024 * 1024)   // Expand by 1MB at a time

// Memory block header with magic number for corruption detection
#define HEAP_MAGIC 0xDEADBEEF
#define HEAP_FREE_MAGIC 0xFEEEBEEF

typedef struct memory_block {
    uint32_t magic;              // Magic number for validation
    uint64_t size;               // Size of usable data (excluding header)
    int is_free;                 // Free flag
    struct memory_block* next;   // Next block
    struct memory_block* prev;   // Previous block for easier coalescing
} __attribute__((packed)) memory_block_t;

// Static heap for GUI mode or fallback
static uint8_t static_heap[INITIAL_HEAP_SIZE] __attribute__((aligned(16)));
static uint64_t static_heap_offset = 0;

// Dynamic heap pointers
uint8_t* heap_start = (uint8_t*)HEAP_START;
uint8_t* heap_end = (uint8_t*)HEAP_START;
uint8_t* heap_current = (uint8_t*)HEAP_START;
static memory_block_t* first_block = NULL;
static memory_block_t* last_block = NULL;

// Heap statistics
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

// System memory info
static uint64_t total_memory_kb = 512 * 1024; // Default 512MB in KB
static int use_static_heap = 0;

// PMM enabled flag
static int pmm_enabled = 0;

// ============================================================================
// PMM (Physical Memory Manager) - Bitmap Implementation
// ============================================================================

#define MAX_MEMORY_SIZE (512 * 1024 * 1024)  // 512MB max
#define BITMAP_SIZE (MAX_MEMORY_SIZE / PAGE_SIZE / 8)

// Bitmap (each bit represents a 4KB frame)
static unsigned char bitmap[BITMAP_SIZE];

// Statistics
static unsigned long total_frames = 0;
static unsigned long used_frames = 0;
static unsigned long free_frames = 0;

// Kernel memory range
#define KERNEL_START 0x100000
#define KERNEL_END   0x500000   // 5MB (1MB start + 4MB kernel)

// Bitmap operations
static inline void bitmap_set_bit(unsigned long frame_index) {
    unsigned long byte_index = frame_index / 8;
    unsigned char bit_index = frame_index % 8;
    bitmap[byte_index] |= (1 << bit_index);
}

static inline void bitmap_clear_bit(unsigned long frame_index) {
    unsigned long byte_index = frame_index / 8;
    unsigned char bit_index = frame_index % 8;
    bitmap[byte_index] &= ~(1 << bit_index);
}

static inline int bitmap_test_bit(unsigned long frame_index) {
    unsigned long byte_index = frame_index / 8;
    unsigned char bit_index = frame_index % 8;
    return (bitmap[byte_index] & (1 << bit_index)) != 0;
}

static inline unsigned long addr_to_frame_index(unsigned long addr) {
    return addr / PAGE_SIZE;
}

static inline unsigned long frame_index_to_addr(unsigned long frame_index) {
    return frame_index * PAGE_SIZE;
}

static void clear_bitmap(void) {
    for (unsigned long i = 0; i < BITMAP_SIZE; i++) {
        bitmap[i] = 0;
    }
}

static void mark_all_used(void) {
    for (unsigned long i = 0; i < BITMAP_SIZE; i++) {
        bitmap[i] = 0xFF;
    }
}

static void mark_region_free(unsigned long base, unsigned long length) {
    unsigned long start_frame = addr_to_frame_index(base);
    unsigned long end_frame = addr_to_frame_index(base + length);
    
    for (unsigned long i = start_frame; i < end_frame; i++) {
        if (i < total_frames) {
            bitmap_clear_bit(i);
            free_frames++;
        }
    }
}

static void mark_region_used(unsigned long base, unsigned long length) {
    unsigned long start_frame = addr_to_frame_index(base);
    unsigned long end_frame = addr_to_frame_index(base + length);
    
    for (unsigned long i = start_frame; i < end_frame; i++) {
        if (i < total_frames) {
            if (!bitmap_test_bit(i)) {
                bitmap_set_bit(i);
                free_frames--;
            }
        }
    }
}

// ============================================================================
// PMM Public Interface
// ============================================================================

void pmm_init(struct memory_map_entry* mmap, unsigned int mmap_count) {
    // Reset statistics
    total_frames = 0;
    used_frames = 0;
    free_frames = 0;
    
    // Clear bitmap
    clear_bitmap();
    
    // Calculate total memory
    unsigned long max_addr = 0;
    for (unsigned int i = 0; i < mmap_count; i++) {
        if (mmap[i].type == 1) {  // Usable memory
            unsigned long end = mmap[i].base + mmap[i].length;
            if (end > max_addr) {
                max_addr = end;
            }
        }
    }
    
    // Limit max memory
    if (max_addr > MAX_MEMORY_SIZE) {
        max_addr = MAX_MEMORY_SIZE;
    }
    
    total_frames = addr_to_frame_index(max_addr);
    
    // Mark all frames as used initially
    mark_all_used();
    used_frames = total_frames;
    free_frames = 0;
    
    // Mark usable memory regions as free
    for (unsigned int i = 0; i < mmap_count; i++) {
        if (mmap[i].type == 1) {  // Usable memory
            unsigned long base = mmap[i].base;
            unsigned long length = mmap[i].length;
            
            if (base < MAX_MEMORY_SIZE) {
                if (base + length > MAX_MEMORY_SIZE) {
                    length = MAX_MEMORY_SIZE - base;
                }
                mark_region_free(base, length);
            }
        }
    }
    
    // Reserve first 1MB
    mark_region_used(0, 0x100000);
    
    // Reserve kernel area
    mark_region_used(KERNEL_START, KERNEL_END - KERNEL_START);
    
    // Reserve bitmap itself
    unsigned long bitmap_addr = KERNEL_END;
    mark_region_used(bitmap_addr, BITMAP_SIZE);
    
    // Fix used_frames count
    used_frames = total_frames - free_frames;
    
    pmm_enabled = 1;
    
    print_str64("PMM Initialized: ", VGA_GREEN);
    
    // Print total memory in MB
    char num_str[32];
    uint64_t total_mb = (total_frames * PAGE_SIZE) / (1024 * 1024);
    int i = 0;
    uint64_t temp = total_mb;
    
    if (temp == 0) {
        num_str[i++] = '0';
    } else {
        while (temp > 0) {
            num_str[i++] = '0' + (temp % 10);
            temp /= 10;
        }
    }
    num_str[i] = '\0';
    
    // Reverse string
    for (int j = 0; j < i / 2; j++) {
        char c = num_str[j];
        num_str[j] = num_str[i - j - 1];
        num_str[i - j - 1] = c;
    }
    
    print_str64(num_str, VGA_YELLOW);
    println64(" MB detected", VGA_GREEN);
}

void* pmm_alloc_frame(void) {
    if (!pmm_enabled) return NULL;
    
    // Find free bit in bitmap
    for (unsigned long i = 0; i < total_frames; i++) {
        if (!bitmap_test_bit(i)) {
            bitmap_set_bit(i);
            used_frames++;
            free_frames--;
            return (void*)frame_index_to_addr(i);
        }
    }
    
    return NULL;
}

void pmm_free_frame(void* frame) {
    if (!pmm_enabled || !frame) return;
    
    unsigned long addr = (unsigned long)frame;
    unsigned long frame_index = addr_to_frame_index(addr);
    
    if (frame_index >= total_frames) return;
    
    if (bitmap_test_bit(frame_index)) {
        bitmap_clear_bit(frame_index);
        used_frames--;
        free_frames++;
    }
}

unsigned long pmm_get_total_memory(void) {
    return total_frames * PAGE_SIZE;
}

unsigned long pmm_get_free_memory(void) {
    return free_frames * PAGE_SIZE;
}

unsigned long pmm_get_used_memory(void) {
    return used_frames * PAGE_SIZE;
}

void pmm_print_stats(void) {
    if (!pmm_enabled) {
        println64("PMM not initialized", VGA_RED);
        return;
    }
    
    println64("\nPhysical Memory Manager Statistics:", VGA_CYAN);
    
    // Total frames
    print_str64("  Total Frames: ", VGA_WHITE);
    char num_str[32];
    int i = 0;
    unsigned long temp = total_frames;
    
    if (temp == 0) {
        num_str[i++] = '0';
    } else {
        while (temp > 0) {
            num_str[i++] = '0' + (temp % 10);
            temp /= 10;
        }
    }
    num_str[i] = '\0';
    
    for (int j = 0; j < i / 2; j++) {
        char c = num_str[j];
        num_str[j] = num_str[i - j - 1];
        num_str[i - j - 1] = c;
    }
    
    println64(num_str, VGA_GREEN);
    
    // Used frames
    print_str64("  Used Frames: ", VGA_WHITE);
    i = 0;
    temp = used_frames;
    
    if (temp == 0) {
        num_str[i++] = '0';
    } else {
        while (temp > 0) {
            num_str[i++] = '0' + (temp % 10);
            temp /= 10;
        }
    }
    num_str[i] = '\0';
    
    for (int j = 0; j < i / 2; j++) {
        char c = num_str[j];
        num_str[j] = num_str[i - j - 1];
        num_str[i - j - 1] = c;
    }
    
    println64(num_str, VGA_GREEN);
    
    // Free frames
    print_str64("  Free Frames: ", VGA_WHITE);
    i = 0;
    temp = free_frames;
    
    if (temp == 0) {
        num_str[i++] = '0';
    } else {
        while (temp > 0) {
            num_str[i++] = '0' + (temp % 10);
            temp /= 10;
        }
    }
    num_str[i] = '\0';
    
    for (int j = 0; j < i / 2; j++) {
        char c = num_str[j];
        num_str[j] = num_str[i - j - 1];
        num_str[i - j - 1] = c;
    }
    
    println64(num_str, VGA_GREEN);
}

// ============================================================================
// Heap Helper Functions
// ============================================================================

// Validate block integrity
static int validate_block(memory_block_t* block) {
    if (!block) return 0;
    return (block->magic == HEAP_MAGIC || 
            (block->is_free && block->magic == HEAP_FREE_MAGIC));
}

// Try to coalesce with next block if both are free
static void coalesce_next(memory_block_t* block) {
    if (!block || !block->next) return;
    if (!block->is_free || !block->next->is_free) return;
    
    memory_block_t* next = block->next;
    
    // Merge blocks
    block->size += sizeof(memory_block_t) + next->size;
    block->next = next->next;
    
    if (next->next) {
        next->next->prev = block;
    } else {
        last_block = block;
    }
    
    heap_stats.coalesces++;
}

// Try to coalesce with previous block if both are free
static void coalesce_prev(memory_block_t* block) {
    if (!block || !block->prev) return;
    if (!block->is_free || !block->prev->is_free) return;
    
    // Let prev absorb this block
    coalesce_next(block->prev);
}

// Expand heap by requesting more memory from PMM
static int expand_heap(uint64_t min_size) {
    if (use_static_heap) return 0; // Can't expand static heap
    
    // Calculate how much to expand
    uint64_t expand_size = HEAP_EXPAND_SIZE;
    if (min_size > expand_size) {
        // Round up to nearest MB
        expand_size = ((min_size + HEAP_EXPAND_SIZE - 1) / HEAP_EXPAND_SIZE) * HEAP_EXPAND_SIZE;
    }
    
    // Check if we've hit max heap size
    uint64_t current_heap_size = (uint64_t)(heap_current - heap_start);
    if (current_heap_size + expand_size > MAX_HEAP_SIZE) {
        return 0; // Can't expand anymore
    }
    
    // In a real system with VMM, we would map new pages here
    // For now, we just extend the heap pointer
    heap_end = heap_current + expand_size;
    heap_stats.heap_expansions++;
    
    return 1;
}

// Split block if it's too large
static void split_block(memory_block_t* block, uint64_t size) {
    // Only split if remainder is big enough for a new block
    uint64_t remainder = block->size - size;
    if (remainder <= sizeof(memory_block_t) + 32) {
        return; // Not worth splitting
    }
    
    // Create new block in the remainder space
    memory_block_t* new_block = (memory_block_t*)((uint8_t*)block + sizeof(memory_block_t) + size);
    new_block->magic = HEAP_FREE_MAGIC;
    new_block->size = remainder - sizeof(memory_block_t);
    new_block->is_free = 1;
    new_block->next = block->next;
    new_block->prev = block;
    
    if (block->next) {
        block->next->prev = new_block;
    } else {
        last_block = new_block;
    }
    
    block->size = size;
    block->next = new_block;
    
    heap_stats.block_splits++;
}

// ============================================================================
// Memory Initialization
// ============================================================================

void init_memory_unified(void) {
    heap_start = (uint8_t*)HEAP_START;
    heap_current = (uint8_t*)HEAP_START;
    heap_end = (uint8_t*)(HEAP_START + INITIAL_HEAP_SIZE);
    first_block = NULL;
    last_block = NULL;
    
    // Reset statistics
    heap_stats.total_allocations = 0;
    heap_stats.total_frees = 0;
    heap_stats.current_allocations = 0;
    heap_stats.bytes_allocated = 0;
    heap_stats.bytes_freed = 0;
    heap_stats.peak_usage = 0;
    heap_stats.heap_expansions = 0;
    heap_stats.coalesces = 0;
    heap_stats.block_splits = 0;
    
    print_str64("  OK Heap initialized: ", VGA_GREEN);
    
    // Print initial heap size in MB
    char num_str[32];
    uint64_t heap_mb = INITIAL_HEAP_SIZE / (1024 * 1024);
    int i = 0;
    uint64_t temp = heap_mb;
    
    while (temp > 0) {
        num_str[i++] = '0' + (temp % 10);
        temp /= 10;
    }
    num_str[i] = '\0';
    
    for (int j = 0; j < i / 2; j++) {
        char c = num_str[j];
        num_str[j] = num_str[i - j - 1];
        num_str[i - j - 1] = c;
    }
    
    print_str64(num_str, VGA_GREEN);
    println64(" MB", VGA_GREEN);
}

uint64_t get_total_memory(void) {
    return total_memory_kb * 1024; // Return in bytes
}

// ============================================================================
// Enhanced Dynamic Memory Allocation
// ============================================================================

void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    
    // Static heap mode (simple bump allocator)
    if (use_static_heap) {
        // Align to 16 bytes
        size = (size + 15) & ~15;
        
        if (static_heap_offset + size > INITIAL_HEAP_SIZE) {
            print_str64("ERROR: Out of static heap memory\n", VGA_RED);
            return NULL;
        }
        
        void* ptr = &static_heap[static_heap_offset];
        static_heap_offset += size;
        return ptr;
    }
    
    // Dynamic heap mode - align to 8 bytes
    size = (size + 7) & ~7;
    
    // First allocation - initialize first block
    if (first_block == NULL) {
        // Check if we have enough space
        if (sizeof(memory_block_t) + size > (uint64_t)(heap_end - heap_current)) {
            if (!expand_heap(sizeof(memory_block_t) + size)) {
                print_str64("ERROR: Out of heap memory\n", VGA_RED);
                return NULL;
            }
        }
        
        first_block = (memory_block_t*)heap_current;
        first_block->magic = HEAP_MAGIC;
        first_block->size = size;
        first_block->is_free = 0;
        first_block->next = NULL;
        first_block->prev = NULL;
        
        last_block = first_block;
        heap_current += sizeof(memory_block_t) + size;
        
        // Update statistics
        heap_stats.total_allocations++;
        heap_stats.current_allocations++;
        heap_stats.bytes_allocated += size;
        if (heap_stats.bytes_allocated > heap_stats.peak_usage) {
            heap_stats.peak_usage = heap_stats.bytes_allocated;
        }
        
        return (void*)((uint8_t*)first_block + sizeof(memory_block_t));
    }
    
    // Search for free block that fits (first fit)
    memory_block_t* block = first_block;
    
    while (block != NULL) {
        if (!validate_block(block)) {
            print_str64("ERROR: Heap corruption detected!\n", VGA_RED);
            return NULL;
        }
        
        if (block->is_free && block->size >= size) {
            // Found suitable block
            block->is_free = 0;
            block->magic = HEAP_MAGIC;
            
            // Try to split if block is much larger than needed
            split_block(block, size);
            
            // Update statistics
            heap_stats.total_allocations++;
            heap_stats.current_allocations++;
            heap_stats.bytes_allocated += size;
            if (heap_stats.bytes_allocated - heap_stats.bytes_freed > heap_stats.peak_usage) {
                heap_stats.peak_usage = heap_stats.bytes_allocated - heap_stats.bytes_freed;
            }
            
            return (void*)((uint8_t*)block + sizeof(memory_block_t));
        }
        
        block = block->next;
    }
    
    // No suitable free block found - allocate new block at end
    uint64_t needed = sizeof(memory_block_t) + size;
    
    if (needed > (uint64_t)(heap_end - heap_current)) {
        if (!expand_heap(needed)) {
            print_str64("ERROR: Out of heap memory\n", VGA_RED);
            return NULL;
        }
    }
    
    memory_block_t* new_block = (memory_block_t*)heap_current;
    new_block->magic = HEAP_MAGIC;
    new_block->size = size;
    new_block->is_free = 0;
    new_block->next = NULL;
    new_block->prev = last_block;
    
    if (last_block) {
        last_block->next = new_block;
    }
    last_block = new_block;
    
    heap_current += needed;
    
    // Update statistics
    heap_stats.total_allocations++;
    heap_stats.current_allocations++;
    heap_stats.bytes_allocated += size;
    if (heap_stats.bytes_allocated - heap_stats.bytes_freed > heap_stats.peak_usage) {
        heap_stats.peak_usage = heap_stats.bytes_allocated - heap_stats.bytes_freed;
    }
    
    return (void*)((uint8_t*)new_block + sizeof(memory_block_t));
}

void kfree(void* ptr) {
    if (ptr == NULL) return;
    
    // Static heap doesn't support free
    if (use_static_heap) {
        return;
    }
    
    // Get block header
    memory_block_t* block = (memory_block_t*)((uint8_t*)ptr - sizeof(memory_block_t));
    
    // Validate block
    if (!validate_block(block)) {
        print_str64("ERROR: Invalid free - heap corruption!\n", VGA_RED);
        return;
    }
    
    if (block->is_free) {
        print_str64("WARNING: Double free detected!\n", VGA_YELLOW);
        return;
    }
    
    // Mark block as free
    block->is_free = 1;
    block->magic = HEAP_FREE_MAGIC;
    
    // Update statistics
    heap_stats.total_frees++;
    heap_stats.current_allocations--;
    heap_stats.bytes_freed += block->size;
    
    // Try to coalesce with adjacent free blocks
    coalesce_next(block);
    coalesce_prev(block);
}

// Reallocate memory block
void* krealloc(void* ptr, size_t new_size) {
    if (ptr == NULL) {
        return kmalloc(new_size);
    }
    
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }
    
    if (use_static_heap) {
        // Static heap can't realloc properly, just alloc new
        void* new_ptr = kmalloc(new_size);
        if (!new_ptr) return NULL;
        
        // Copy old data (assume smaller size)
        uint8_t* src = (uint8_t*)ptr;
        uint8_t* dst = (uint8_t*)new_ptr;
        for (size_t i = 0; i < new_size; i++) {
            dst[i] = src[i];
        }
        
        return new_ptr;
    }
    
    // Get current block
    memory_block_t* block = (memory_block_t*)((uint8_t*)ptr - sizeof(memory_block_t));
    
    if (!validate_block(block)) {
        return NULL;
    }
    
    // If new size fits in current block, just return it
    if (new_size <= block->size) {
        // Could shrink block here if significantly smaller
        return ptr;
    }
    
    // Check if we can expand into next block if it's free
    if (block->next && block->next->is_free) {
        uint64_t combined_size = block->size + sizeof(memory_block_t) + block->next->size;
        if (combined_size >= new_size) {
            // Absorb next block
            coalesce_next(block);
            return ptr;
        }
    }
    
    // Need to allocate new block
    void* new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;
    
    // Copy old data
    uint8_t* src = (uint8_t*)ptr;
    uint8_t* dst = (uint8_t*)new_ptr;
    for (uint64_t i = 0; i < block->size; i++) {
        dst[i] = src[i];
    }
    
    // Free old block
    kfree(ptr);
    
    return new_ptr;
}

// Allocate and zero memory
void* kcalloc(size_t num, size_t size) {
    uint64_t total_size = num * size;
    void* ptr = kmalloc(total_size);
    
    if (ptr) {
        uint8_t* p = (uint8_t*)ptr;
        for (uint64_t i = 0; i < total_size; i++) {
            p[i] = 0;
        }
    }
    
    return ptr;
}

// ============================================================================
// SBRK Interface – SYS_SBRK syscall icin
// heap_current: son alloc pointer'i (brk degeri olarak kullanilir)
// ============================================================================

uint64_t kmalloc_get_brk(void) {
    return (uint64_t)heap_current;
}

// new_brk: heap_current'ten buyuk olmali, MAX_HEAP_SIZE'i asmamali.
// Donus: yeni brk | (uint64_t)-1 hata
uint64_t kmalloc_set_brk(uint64_t new_brk) {
    if (new_brk < (uint64_t)heap_current)                    return (uint64_t)-1;
    if (new_brk > (uint64_t)heap_start + MAX_HEAP_SIZE)      return (uint64_t)-1;
    if (new_brk > (uint64_t)heap_end) {
        heap_end = (uint8_t*)new_brk;
        heap_stats.heap_expansions++;
    }
    heap_current = (uint8_t*)new_brk;
    return (uint64_t)heap_current;
}

// ============================================================================
// GUI-Compatible Aliases
// ============================================================================

void* malloc_gui(uint64_t size) {
    return kmalloc((size_t)size);
}

void free_gui(void* ptr) {
    kfree(ptr);
}

// ============================================================================
// Page Mapping
// ============================================================================

void* map_page(uint64_t physical, uint64_t virtual __attribute__((unused))) {
    // Simplified - identity mapping only
    return (void*)physical;
}

// ============================================================================
// Memory Statistics
// ============================================================================

void show_memory_info(void) {
    if (use_static_heap) {
        println64("\n=== Memory Statistics (Static Heap) ===", VGA_CYAN);
        print_str64("  Heap Size: ", VGA_WHITE);
        
        // Print heap size in KB
        uint64_t heap_kb = INITIAL_HEAP_SIZE / 1024;
        char num_str[32];
        int i = 0;
        uint64_t temp = heap_kb;
        
        while (temp > 0) {
            num_str[i++] = '0' + (temp % 10);
            temp /= 10;
        }
        num_str[i] = '\0';
        
        for (int j = 0; j < i / 2; j++) {
            char c = num_str[j];
            num_str[j] = num_str[i - j - 1];
            num_str[i - j - 1] = c;
        }
        
        print_str64(num_str, VGA_GREEN);
        println64(" KB", VGA_GREEN);
        
        print_str64("  Used: ", VGA_WHITE);
        
        // Print used memory
        uint64_t used_kb = static_heap_offset / 1024;
        i = 0;
        temp = used_kb;
        
        if (temp == 0) {
            num_str[i++] = '0';
        } else {
            while (temp > 0) {
                num_str[i++] = '0' + (temp % 10);
                temp /= 10;
            }
        }
        num_str[i] = '\0';
        
        for (int j = 0; j < i / 2; j++) {
            char c = num_str[j];
            num_str[j] = num_str[i - j - 1];
            num_str[i - j - 1] = c;
        }
        
        print_str64(num_str, VGA_GREEN);
        println64(" KB", VGA_GREEN);
        return;
    }
    
    // Dynamic heap statistics
    println64("\n=== Memory Statistics (Dynamic Heap) ===", VGA_CYAN);
    
    // Total allocations
    print_str64("  Total Allocations: ", VGA_WHITE);
    char num_str[32];
    int i = 0;
    uint64_t temp = heap_stats.total_allocations;
    
    if (temp == 0) {
        num_str[i++] = '0';
    } else {
        while (temp > 0) {
            num_str[i++] = '0' + (temp % 10);
            temp /= 10;
        }
    }
    num_str[i] = '\0';
    for (int j = 0; j < i / 2; j++) {
        char c = num_str[j];
        num_str[j] = num_str[i - j - 1];
        num_str[i - j - 1] = c;
    }
    println64(num_str, VGA_GREEN);
    
    // Total frees
    print_str64("  Total Frees: ", VGA_WHITE);
    i = 0;
    temp = heap_stats.total_frees;
    if (temp == 0) {
        num_str[i++] = '0';
    } else {
        while (temp > 0) {
            num_str[i++] = '0' + (temp % 10);
            temp /= 10;
        }
    }
    num_str[i] = '\0';
    for (int j = 0; j < i / 2; j++) {
        char c = num_str[j];
        num_str[j] = num_str[i - j - 1];
        num_str[i - j - 1] = c;
    }
    println64(num_str, VGA_YELLOW);
    
    // Current allocations
    print_str64("  Active Allocations: ", VGA_WHITE);
    i = 0;
    temp = heap_stats.current_allocations;
    if (temp == 0) {
        num_str[i++] = '0';
    } else {
        while (temp > 0) {
            num_str[i++] = '0' + (temp % 10);
            temp /= 10;
        }
    }
    num_str[i] = '\0';
    for (int j = 0; j < i / 2; j++) {
        char c = num_str[j];
        num_str[j] = num_str[i - j - 1];
        num_str[i - j - 1] = c;
    }
    println64(num_str, VGA_CYAN);
    
    // Peak usage
    print_str64("  Peak Usage: ", VGA_WHITE);
    uint64_t peak_kb = heap_stats.peak_usage / 1024;
    i = 0;
    temp = peak_kb;
    if (temp == 0) {
        num_str[i++] = '0';
    } else {
        while (temp > 0) {
            num_str[i++] = '0' + (temp % 10);
            temp /= 10;
        }
    }
    num_str[i] = '\0';
    for (int j = 0; j < i / 2; j++) {
        char c = num_str[j];
        num_str[j] = num_str[i - j - 1];
        num_str[i - j - 1] = c;
    }
    print_str64(num_str, VGA_GREEN);
    println64(" KB", VGA_GREEN);
    
    // Heap expansions
    print_str64("  Heap Expansions: ", VGA_WHITE);
    i = 0;
    temp = heap_stats.heap_expansions;
    if (temp == 0) {
        num_str[i++] = '0';
    } else {
        while (temp > 0) {
            num_str[i++] = '0' + (temp % 10);
            temp /= 10;
        }
    }
    num_str[i] = '\0';
    for (int j = 0; j < i / 2; j++) {
        char c = num_str[j];
        num_str[j] = num_str[i - j - 1];
        num_str[i - j - 1] = c;
    }
    println64(num_str, VGA_YELLOW);
    
    // Coalesces
    print_str64("  Block Coalesces: ", VGA_WHITE);
    i = 0;
    temp = heap_stats.coalesces;
    if (temp == 0) {
        num_str[i++] = '0';
    } else {
        while (temp > 0) {
            num_str[i++] = '0' + (temp % 10);
            temp /= 10;
        }
    }
    num_str[i] = '\0';
    for (int j = 0; j < i / 2; j++) {
        char c = num_str[j];
        num_str[j] = num_str[i - j - 1];
        num_str[i - j - 1] = c;
    }
    println64(num_str, VGA_CYAN);
    
    // Fragmentation info
    uint64_t total_blocks = 0;
    uint64_t free_blocks = 0;
    memory_block_t* block = first_block;
    while (block) {
        total_blocks++;
        if (block->is_free) free_blocks++;
        block = block->next;
    }
    
    print_str64("  Total Blocks: ", VGA_WHITE);
    i = 0;
    temp = total_blocks;
    if (temp == 0) {
        num_str[i++] = '0';
    } else {
        while (temp > 0) {
            num_str[i++] = '0' + (temp % 10);
            temp /= 10;
        }
    }
    num_str[i] = '\0';
    for (int j = 0; j < i / 2; j++) {
        char c = num_str[j];
        num_str[j] = num_str[i - j - 1];
        num_str[i - j - 1] = c;
    }
    print_str64(num_str, VGA_GREEN);
    
    print_str64(" (", VGA_WHITE);
    i = 0;
    temp = free_blocks;
    if (temp == 0) {
        num_str[i++] = '0';
    } else {
        while (temp > 0) {
            num_str[i++] = '0' + (temp % 10);
            temp /= 10;
        }
    }
    num_str[i] = '\0';
    for (int j = 0; j < i / 2; j++) {
        char c = num_str[j];
        num_str[j] = num_str[i - j - 1];
        num_str[i - j - 1] = c;
    }
    print_str64(num_str, VGA_YELLOW);
    println64(" free)", VGA_WHITE);
}

// ============================================================================
// Mode Control
// ============================================================================

void set_static_heap_mode(int enable) {
    use_static_heap = enable;
    if (enable) {
        println64("Switched to static heap mode", VGA_YELLOW);
    } else {
        println64("Switched to dynamic heap mode", VGA_YELLOW);
    }
}

// ============================================================================
// Backward Compatibility Aliases
// ============================================================================

void init_memory64(void) {
    init_memory_unified();
}

void init_memory_gui(void) {
    init_memory_unified();
}