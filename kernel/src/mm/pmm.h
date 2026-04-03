#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include <limine.h>

#define PAGE_SIZE 4096

// Initialize the physical memory manager using the memory map and HHDM offset.
void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset);

// Allocate a single contiguous physical page. Returns NULL on failure.
void *pmm_alloc(void);

// Allocate multiple contiguous physical pages. Returns NULL on failure.
void *pmm_alloc_blocks(size_t count);

// Free a previously allocated physical page.
void pmm_free(void *ptr);

// Free multiple contiguous physical pages.
void pmm_free_blocks(void *ptr, size_t count);

// Reclaims the memory occupied by the Limine bootloader after boot structures are no longer needed
void pmm_reclaim_bootloader(void);

// Return usable memory in bytes
uint64_t pmm_get_usable_memory(void);

// Return total generic memory in bytes
uint64_t pmm_get_total_memory(void);

// Expose the HHDM base
uint64_t pmm_get_hhdm_offset(void);

#endif
