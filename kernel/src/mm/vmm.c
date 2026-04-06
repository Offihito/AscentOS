#include "mm/vmm.h"
#include "mm/pmm.h"
#include "console/klog.h"
#include <stdint.h>
#include <stddef.h>
#define PHYS_TO_VIRT(p) ((void*)((uint64_t)(p) + pmm_get_hhdm_offset()))
#define PAGE_MASK 0x000FFFFFFFFFF000ULL

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
    uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)kernel_pml4);
    
    // The bootloader (Limine) leaves an identity map of the lower 4GB in the lower half
    // (PML4 entries 0-255). Since the kernel runs in the higher half (-2GB) and Limine
    // structures are in the HHDM, we must unmap the lower half to avoid conflicts
    // when mapping user space, especially avoiding 1GB huge-page aliasing bugs.
    for (int i = 0; i < 256; i++) {
        pml4_virt[i] = 0;
    }
    
    // Flush TLB
    __asm__ volatile("mov %0, %%cr3" :: "r"(kernel_pml4) : "memory");
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

bool vmm_map_page(uint64_t *pml4, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    spinlock_acquire(&vmm_lock);
    bool success = false;

    // Determine the indices for each page table level
    size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
    size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
    size_t pd_index   = (virtual_addr >> 21) & 0x1FF;
    size_t pt_index   = (virtual_addr >> 12) & 0x1FF;

    uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4);

    // Ensure propagation of User and RW flags to higher levels if requested
    uint64_t propagate_flags = flags & (PAGE_FLAG_USER | PAGE_FLAG_RW);

    // Get PDPT (or create if missing)
    uint64_t *pdpt_virt = get_next_level(pml4_virt, pml4_index, true);
    if (!pdpt_virt) {
        klog_puts("[VMM] Error: Failed to get/create PDPT for vaddr 0x");
        klog_uint64(virtual_addr);
        klog_puts("\n");
        goto unlock;
    }
    pml4_virt[pml4_index] |= propagate_flags;

    // Get PD (or create if missing)
    pdpt_virt[pdpt_index] |= propagate_flags;
    uint64_t *pd_virt = get_next_level(pdpt_virt, pdpt_index, true);
    if (!pd_virt) {
        klog_puts("[VMM] Error: Failed to get/create PD for vaddr 0x");
        klog_uint64(virtual_addr);
        klog_puts("\n");
        goto unlock;
    }

    // Get PT (or create if missing)
    pd_virt[pd_index] |= propagate_flags;
    uint64_t *pt_virt = get_next_level(pd_virt, pd_index, true);
    if (!pt_virt) {
        klog_puts("[VMM] Error: Failed to get/create PT for vaddr 0x");
        klog_uint64(virtual_addr);
        klog_puts("\n");
        goto unlock;
    }

    // Set the page entry
    pt_virt[pt_index] = (physical_addr & PAGE_MASK) | flags | PAGE_FLAG_PRESENT;
    vmm_flush_tlb(virtual_addr);
    success = true;

unlock:
    spinlock_release(&vmm_lock);
    return success;
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

uint64_t vmm_virt_to_phys(uint64_t *pml4_phys, uint64_t virtual_addr) {
    size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
    size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
    size_t pd_index   = (virtual_addr >> 21) & 0x1FF;
    size_t pt_index   = (virtual_addr >> 12) & 0x1FF;

    uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4_phys);

    if (!(pml4_virt[pml4_index] & PAGE_FLAG_PRESENT)) return 0;
    uint64_t *pdpt_virt = (uint64_t *)PHYS_TO_VIRT(pml4_virt[pml4_index] & PAGE_MASK);

    if (!(pdpt_virt[pdpt_index] & PAGE_FLAG_PRESENT)) return 0;
    uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(pdpt_virt[pdpt_index] & PAGE_MASK);

    if (!(pd_virt[pd_index] & PAGE_FLAG_PRESENT)) return 0;
    uint64_t *pt_virt = (uint64_t *)PHYS_TO_VIRT(pd_virt[pd_index] & PAGE_MASK);

    if (!(pt_virt[pt_index] & PAGE_FLAG_PRESENT)) return 0;
    return (pt_virt[pt_index] & PAGE_MASK) | (virtual_addr & 0xFFF);
}

// ── Deep-copy helper for page table cloning ─────────────────────────────────
// Recursively clone page table levels.  At level 1 (leaf PT) we allocate
// fresh physical pages and copy their content.  At higher levels we allocate
// new table pages and recurse.
//
// level: 4 = PML4, 3 = PDPT, 2 = PD, 1 = PT
// start / end: range of entries to process (0-511 for full clone)
static uint64_t *clone_table(uint64_t *src_table_phys, int level,
                              size_t start, size_t end) {
    void *new_table_phys = pmm_alloc();
    if (!new_table_phys) return NULL;

    uint64_t *new_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_table_phys);
    uint64_t *src_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)src_table_phys);

    // Zero the new table first
    for (size_t i = 0; i < 512; i++) {
        new_virt[i] = 0;
    }

    for (size_t i = start; i < end; i++) {
        if (!(src_virt[i] & PAGE_FLAG_PRESENT)) continue;
        if (!(src_virt[i] & PAGE_FLAG_USER)) continue; // Ignore kernel/Limine mappings

        if (level == 1) {
            // Leaf level: allocate a fresh physical page and copy content
            void *new_page_phys = pmm_alloc();
            if (!new_page_phys) return NULL; // OOM

            uint8_t *dst = (uint8_t *)PHYS_TO_VIRT((uint64_t)new_page_phys);
            uint8_t *src = (uint8_t *)PHYS_TO_VIRT(src_virt[i] & PAGE_MASK);

            // Copy page content using 64-bit words for speed
            uint64_t *dst64 = (uint64_t *)dst;
            uint64_t *src64 = (uint64_t *)src;
            for (size_t w = 0; w < 512; w++) {
                dst64[w] = src64[w];
            }

            // Preserve flags from original PTE
            new_virt[i] = ((uint64_t)new_page_phys & PAGE_MASK) |
                          (src_virt[i] & ~PAGE_MASK);
        } else {
            // Intermediate level: recurse
            uint64_t *child_src_phys = (uint64_t *)(src_virt[i] & PAGE_MASK);
            uint64_t *child_new_phys = clone_table(child_src_phys, level - 1,
                                                    0, 512);
            if (!child_new_phys) return NULL; // OOM

            // Preserve flags from original entry
            new_virt[i] = ((uint64_t)child_new_phys & PAGE_MASK) |
                          (src_virt[i] & ~PAGE_MASK);
        }
    }

    return (uint64_t *)new_table_phys; // Return physical address
}

uint64_t vmm_clone_user_mappings(uint64_t *src_pml4_phys) {
    spinlock_acquire(&vmm_lock);

    // Allocate a new PML4
    void *new_pml4_phys = pmm_alloc();
    if (!new_pml4_phys) {
        spinlock_release(&vmm_lock);
        return 0;
    }

    uint64_t *new_pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_pml4_phys);
    uint64_t *src_pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)src_pml4_phys);

    // Zero the new PML4
    for (size_t i = 0; i < 512; i++) {
        new_pml4_virt[i] = 0;
    }

    // Clone user-space entries (0-255): deep copy
    for (size_t i = 0; i < 256; i++) {
        if (!(src_pml4_virt[i] & PAGE_FLAG_PRESENT)) continue;
        if (!(src_pml4_virt[i] & PAGE_FLAG_USER)) continue; // Ignore kernel/Limine mappings

        uint64_t *child_src_phys = (uint64_t *)(src_pml4_virt[i] & PAGE_MASK);
        uint64_t *child_new_phys = clone_table(child_src_phys, 3, 0, 512);
        if (!child_new_phys) {
            // OOM — we should free what we allocated, but for simplicity
            // just fail. A real OS would roll back.
            spinlock_release(&vmm_lock);
            return 0;
        }

        new_pml4_virt[i] = ((uint64_t)child_new_phys & PAGE_MASK) |
                           (src_pml4_virt[i] & ~PAGE_MASK);
    }

    // Shallow-copy kernel higher-half entries (256-511): share the same
    // kernel page tables between parent and child.
    for (size_t i = 256; i < 512; i++) {
        new_pml4_virt[i] = src_pml4_virt[i];
    }

    spinlock_release(&vmm_lock);
    return (uint64_t)new_pml4_phys;
}

uint64_t *vmm_create_pml4(void) {
    spinlock_acquire(&vmm_lock);

    void *new_pml4_phys = pmm_alloc();
    if (!new_pml4_phys) {
        spinlock_release(&vmm_lock);
        return NULL;
    }

    uint64_t *new_pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_pml4_phys);
    uint64_t *src_pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)kernel_pml4);

    // Zero the user half (0-255)
    for (size_t i = 0; i < 256; i++) {
        new_pml4_virt[i] = 0;
    }

    // Shallow-copy the kernel half (256-511)
    for (size_t i = 256; i < 512; i++) {
        new_pml4_virt[i] = src_pml4_virt[i];
    }

    spinlock_release(&vmm_lock);
    return (uint64_t *)new_pml4_phys;
}
