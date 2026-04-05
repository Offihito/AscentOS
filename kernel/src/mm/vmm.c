#include "mm/vmm.h"
#include "mm/pmm.h"
#include <stdint.h>
#include <stddef.h>
#define PHYS_TO_VIRT(p) ((void*)((uint64_t)(p) + pmm_get_hhdm_offset()))
#define PAGE_MASK 0xFFFFFFFFFFFFF000

#include "lock/spinlock.h"
static spinlock_t vmm_lock = SPINLOCK_INIT;

static uint64_t *kernel_pml4 = NULL;

uint64_t *vmm_get_active_pml4(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (uint64_t *)(cr3 & PAGE_MASK);
}

void vmm_init(void) {
    kernel_pml4 = vmm_get_active_pml4();
}

static uint64_t *get_next_level(uint64_t *current_level, size_t index, bool allocate) {
    if (current_level[index] & PAGE_FLAG_PRESENT) {
        // Table exists, return its virtual address
        uint64_t next_phys = current_level[index] & PAGE_MASK;
        return (uint64_t *)PHYS_TO_VIRT(next_phys);
    }
    
    if (!allocate) {
        return NULL;
    }
    
    // Allocate a new table
    void *new_table_phys = pmm_alloc();
    if (!new_table_phys) {
        return NULL; // Out of Memory
    }
    
    // Zero out the newly allocated page
    uint64_t *new_table_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_table_phys);
    for (size_t i = 0; i < 512; i++) {
        new_table_virt[i] = 0;
    }
    
    // Link it. User/RW permission is granted if the entire chain has it.
    current_level[index] = ((uint64_t)new_table_phys) | PAGE_FLAG_PRESENT | PAGE_FLAG_RW | PAGE_FLAG_USER;
    
    return new_table_virt;
}

void vmm_map_page(uint64_t *pml4, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    spinlock_acquire(&vmm_lock);

    // Determine the indices for each page table level
    size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
    size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
    size_t pd_index   = (virtual_addr >> 21) & 0x1FF;
    size_t pt_index   = (virtual_addr >> 12) & 0x1FF;

    uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4);

    // Ensure propagation of User and RW flags to higher levels if requested
    uint64_t propagate_flags = flags & (PAGE_FLAG_USER | PAGE_FLAG_RW);

    pml4_virt[pml4_index] |= propagate_flags;
    uint64_t *pdpt_virt = get_next_level(pml4_virt, pml4_index, true);
    if (!pdpt_virt) goto unlock;

    pdpt_virt[pdpt_index] |= propagate_flags;
    uint64_t *pd_virt = get_next_level(pdpt_virt, pdpt_index, true);
    if (!pd_virt) goto unlock;

    pd_virt[pd_index] |= propagate_flags;
    uint64_t *pt_virt = get_next_level(pd_virt, pd_index, true);
    if (!pt_virt) goto unlock;

    pt_virt[pt_index] = (physical_addr & PAGE_MASK) | flags | PAGE_FLAG_PRESENT;
    vmm_flush_tlb(virtual_addr);

unlock:
    spinlock_release(&vmm_lock);
}

void vmm_unmap_page(uint64_t *pml4, uint64_t virtual_addr) {
    spinlock_acquire(&vmm_lock);

    size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
    size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
    size_t pd_index   = (virtual_addr >> 21) & 0x1FF;
    size_t pt_index   = (virtual_addr >> 12) & 0x1FF;

    uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4);

    uint64_t *pdpt_virt = get_next_level(pml4_virt, pml4_index, false);
    if (!pdpt_virt) goto unlock;

    uint64_t *pd_virt = get_next_level(pdpt_virt, pdpt_index, false);
    if (!pd_virt) goto unlock;

    uint64_t *pt_virt = get_next_level(pd_virt, pd_index, false);
    if (!pt_virt) goto unlock;

    pt_virt[pt_index] = 0; // Clear the entry
    vmm_flush_tlb(virtual_addr);

unlock:
    spinlock_release(&vmm_lock);
}
