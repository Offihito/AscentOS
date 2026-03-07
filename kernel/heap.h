// heap.h - Enhanced Kernel Heap Allocator (kmalloc / kfree / krealloc / kcalloc)
#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// Heap layout constants (shared with vmm / boot code if needed)
// ============================================================================

#define PAGE_SIZE          4096
#define INITIAL_HEAP_SIZE  (4  * 1024 * 1024)   // 4 MB  – starting window
#define MAX_HEAP_SIZE      (64 * 1024 * 1024)   // 64 MB – hard ceiling
#define HEAP_START         0x200000              // 2 MB physical
#define HEAP_EXPAND_SIZE   (1  * 1024 * 1024)   // expand granularity

// ============================================================================
// Initialization
// ============================================================================

void init_heap(void);       // primary entry point
void init_memory_unified(void); // backward-compat alias  (→ init_heap)
void init_memory64(void);       // backward-compat alias  (→ init_heap)
void init_memory_gui(void);     // backward-compat alias  (→ init_heap)

// ============================================================================
// Core allocator API
// ============================================================================

void* kmalloc (size_t size);
void  kfree   (void*  ptr);
void* krealloc(void*  ptr,  size_t new_size);
void* kcalloc (size_t num,  size_t size);

// ============================================================================
// GUI-compatible aliases
// ============================================================================

void* malloc_gui(uint64_t size);
void  free_gui  (void*    ptr);

// ============================================================================
// sbrk / brk interface (SYS_SBRK syscall support)
// ============================================================================

uint64_t kmalloc_get_brk(void);
uint64_t kmalloc_set_brk(uint64_t new_brk);

// ============================================================================
// Statistics & debug
// ============================================================================

void show_memory_info  (void);
void set_static_heap_mode(int enable);

// ============================================================================
// Exported pointers (legacy consumers)
// ============================================================================

extern uint8_t* heap_start;
extern uint8_t* heap_current;

// ============================================================================
// Misc helpers
// ============================================================================

void*    map_page       (uint64_t physical, uint64_t virtual_addr);
uint64_t get_total_memory(void);

#endif // HEAP_H