// memory_unified.h - Unified Memory Management System with Enhanced Heap
#ifndef MEMORY_UNIFIED_H
#define MEMORY_UNIFIED_H

#include <stdint.h>
#include <stddef.h>

// Page size
#define PAGE_SIZE 4096

// Memory initialization
void init_memory_unified(void);

// Get total system memory in bytes
uint64_t get_total_memory(void);

// ============================================================================
// Enhanced Dynamic Memory Allocation (Primary Interface)
// ============================================================================

// Allocate memory block
void* kmalloc(size_t size);

// Free memory block
void kfree(void* ptr);

// Reallocate memory block (resize)
void* krealloc(void* ptr, size_t new_size);

// Allocate and zero-initialize memory
void* kcalloc(size_t num, size_t size);

// ============================================================================
// GUI-compatible aliases
// ============================================================================

void* malloc_gui(uint64_t size);
void free_gui(void* ptr);

// ============================================================================
// Page mapping
// ============================================================================

void* map_page(uint64_t physical, uint64_t virtual);

// ============================================================================
// Memory statistics and debugging
// ============================================================================

// Show detailed memory statistics
void show_memory_info(void);

// Mode control (static vs dynamic heap)
void set_static_heap_mode(int enable);

// ============================================================================
// Backward compatibility aliases
// ============================================================================

void init_memory64(void);      // Alias for init_memory_unified
void init_memory_gui(void);    // Alias for init_memory_unified

// Exported variables for backward compatibility
extern uint8_t* heap_start;
extern uint8_t* heap_current;

// ============================================================================
// PMM (Physical Memory Manager) Integration
// ============================================================================

// Memory map entry from BIOS E820
struct memory_map_entry {
    unsigned long base;
    unsigned long length;
    unsigned int type;
    unsigned int acpi_extended;
} __attribute__((packed));

// PMM initialization
void pmm_init(struct memory_map_entry* mmap, unsigned int mmap_count);

// Frame allocation/deallocation
void* pmm_alloc_frame(void);
void pmm_free_frame(void* frame);

// PMM utility functions
unsigned long pmm_get_total_memory(void);
unsigned long pmm_get_free_memory(void);
unsigned long pmm_get_used_memory(void);

// PMM debug/statistics
void pmm_print_stats(void);

#endif // MEMORY_UNIFIED_H