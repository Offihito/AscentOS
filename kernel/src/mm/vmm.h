#ifndef VMM_H
#define VMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGE_FLAG_PRESENT ((uint64_t)1 << 0)
#define PAGE_FLAG_RW ((uint64_t)1 << 1)
#define PAGE_FLAG_USER ((uint64_t)1 << 2)
#define PAGE_FLAG_NX ((uint64_t)1 << 63)

// To retrieve the active top-level page directory from CR3
uint64_t *vmm_get_active_pml4(void);

// Given the active PML4 and a virtual address, map it to a physical frame
// Returns true on success, false on failure (OOM allocating intermediate page tables)
bool vmm_map_page(uint64_t *pml4, uint64_t virtual_addr, uint64_t physical_addr,
                  uint64_t flags);

// Unmap a virtual page
void vmm_unmap_page(uint64_t *pml4, uint64_t virtual_addr);

// Flush the Translation Lookaside Buffer for a specific page
static inline void vmm_flush_tlb(uint64_t virtual_addr) {
  __asm__ volatile("invlpg (%0)" ::"r"(virtual_addr) : "memory");
}

// Resolve a virtual address to its physical address using the given PML4.
// Returns 0 if the mapping does not exist.
uint64_t vmm_virt_to_phys(uint64_t *pml4, uint64_t virtual_addr);

// Clone all user-space page mappings (PML4 entries 0-255) from src_pml4
// into a newly allocated PML4. Kernel higher-half entries (256-511) are
// shallow-copied (shared). Each mapped user page gets a fresh physical
// frame with its content copied. Returns the *physical* address of the
// new PML4, or 0 on failure.
uint64_t vmm_clone_user_mappings(uint64_t *src_pml4_phys);

// Clone user-space page mappings with VMA awareness.
// Shared mappings (MAP_SHARED) share physical pages between parent and child.
// Private mappings (MAP_PRIVATE) get deep-copied (child gets its own pages).
// Returns the *physical* address of the new PML4, or 0 on failure.
struct vma_list;
uint64_t vmm_clone_user_mappings_vma(uint64_t *src_pml4_phys, struct vma_list *vmas);

// Create a new blank address space (shallow copy of kernel-space only)
uint64_t *vmm_create_pml4(void);

// Initialize the base system kernel map
void vmm_init(void);

#endif
