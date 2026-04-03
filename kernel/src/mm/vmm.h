#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PAGE_FLAG_PRESENT ((uint64_t)1 << 0)
#define PAGE_FLAG_RW      ((uint64_t)1 << 1)
#define PAGE_FLAG_USER    ((uint64_t)1 << 2)
#define PAGE_FLAG_NX      ((uint64_t)1 << 63)

// To retrieve the active top-level page directory from CR3
uint64_t *vmm_get_active_pml4(void);

// Given the active PML4 and a virtual address, map it to a physical frame
void vmm_map_page(uint64_t *pml4, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);

// Unmap a virtual page
void vmm_unmap_page(uint64_t *pml4, uint64_t virtual_addr);

// Flush the Translation Lookaside Buffer for a specific page
static inline void vmm_flush_tlb(uint64_t virtual_addr) {
    __asm__ volatile("invlpg (%0)" :: "r"(virtual_addr) : "memory");
}

// Initialize the base system kernel map
void vmm_init(void);

#endif
