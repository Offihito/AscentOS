// memory_gui.c - Simple memory management for GUI mode
#include <stdint.h>

// Simple memory detection using multiboot info
static uint64_t total_memory_kb = 512 * 1024; // Default 512MB in KB

void init_memory_gui(void) {
    // For now, we assume 512MB
    // In a real system, you'd parse multiboot memory map
    total_memory_kb = 512 * 1024;
}

uint64_t get_total_memory(void) {
    return total_memory_kb * 1024; // Return in bytes
}

// Simple allocator for GUI (static pool)
#define HEAP_SIZE (4 * 1024 * 1024) // 4MB heap
static uint8_t heap[HEAP_SIZE] __attribute__((aligned(16)));
static uint64_t heap_offset = 0;

void* malloc_gui(uint64_t size) {
    // Align to 16 bytes
    size = (size + 15) & ~15;
    
    if (heap_offset + size > HEAP_SIZE) {
        return (void*)0; // Out of memory
    }
    
    void* ptr = &heap[heap_offset];
    heap_offset += size;
    return ptr;
}

void free_gui(void* ptr) {
    (void)ptr;
    // Simple allocator doesn't support free
    // In a real system, you'd implement a proper allocator
}