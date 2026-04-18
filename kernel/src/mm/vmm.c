#include "vmm.h"
#include "../console/klog.h"
#include "pmm.h"
#include "vma.h"
#include <stddef.h>
#include <stdint.h>
#define PHYS_TO_VIRT(p) ((void *)((uint64_t)(p) + pmm_get_hhdm_offset()))

#include "../lock/spinlock.h"
static spinlock_t vmm_lock = SPINLOCK_INIT;

static uint64_t *kernel_pml4 = NULL;

uint64_t *vmm_get_active_pml4(void) {
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  return (uint64_t *)(cr3 & PAGE_MASK);
}

// Recursively deep-copy a page table tree to decouple it from bootloader
// memory. Level 1: leaf page table (4KB pages). Level 2/3: might be huge pages
// (2MB/1GB) or sub-tables.
static uint64_t vmm_deep_clone_table(uint64_t src_phys, int level) {
  if (level < 1)
    return 0;

  void *new_page_phys = pmm_alloc();
  if (!new_page_phys)
    return 0;
  uint64_t *new_table_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_page_phys);
  uint64_t *src_table_virt = (uint64_t *)PHYS_TO_VIRT(src_phys);

  for (int i = 0; i < 512; i++) {
    uint64_t entry = src_table_virt[i];
    if (!(entry & PAGE_FLAG_PRESENT)) {
      new_table_virt[i] = 0;
      continue;
    }

    // If it's a huge page (at level 2 or 3) or we've reached the bottom level
    // (1), then this is a leaf mapping, not a sub-table. Copy the mapping
    // itself.
    if ((level > 1 && (entry & PAGE_FLAG_PS)) || level == 1) {
      new_table_virt[i] = entry;
      continue;
    }

    // Recurse to clone the sub-table.
    uint64_t sub_phys = entry & PAGE_MASK;
    uint64_t new_sub_phys = vmm_deep_clone_table(sub_phys, level - 1);
    if (!new_sub_phys) {
      // In a real kernel, we would free already-allocated pages here.
      return 0;
    }
    new_table_virt[i] = (new_sub_phys & PAGE_MASK) | (entry & ~PAGE_MASK);
  }

  return (uint64_t)new_page_phys;
}

void vmm_init(void) {
  // 1. Identify and protect the current (boot) page tables in PMM.
  kernel_pml4 = vmm_get_active_pml4();
  pmm_mark_used(kernel_pml4, 1);

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)kernel_pml4);

  // 2. Clear out Limine's lower-half identity mapping.
  for (int i = 0; i < 256; i++) {
    pml4_virt[i] = 0;
  }

  // 3. Create a fresh, private PML4 and DEEP-COPY the kernel half.
  // This is vital! Limine places its tables in reclaimable memory.
  void *new_pml4_phys = pmm_alloc();
  if (!new_pml4_phys) {
    klog_puts("[VMM] FATAL: Failed to allocate new kernel PML4\n");
    return;
  }
  uint64_t *new_pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_pml4_phys);
  for (int i = 0; i < 512; i++)
    new_pml4_virt[i] = 0;

  for (int i = 256; i < 512; i++) {
    uint64_t entry = pml4_virt[i];
    if (entry & PAGE_FLAG_PRESENT) {
      if (entry & PAGE_FLAG_PS) {
        new_pml4_virt[i] = entry;
      } else {
        uint64_t cloned = vmm_deep_clone_table(entry & PAGE_MASK, 3);
        new_pml4_virt[i] = (cloned & PAGE_MASK) | (entry & ~PAGE_MASK);
      }
    }
  }

  kernel_pml4 = (uint64_t *)new_pml4_phys;

  // Switch to the newly allocated kernel-owned PML4
  __asm__ volatile("mov %0, %%cr3" ::"r"(kernel_pml4) : "memory");
  klog_puts("[VMM] Switched to new, independent kernel-owned PML4.\n");
}

static uint64_t *get_next_level(uint64_t *current_level, size_t index,
                                bool allocate) {
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
  current_level[index] = ((uint64_t)new_table_phys) | PAGE_FLAG_PRESENT |
                         PAGE_FLAG_RW | PAGE_FLAG_USER;

  return new_table_virt;
}

bool vmm_map_page(uint64_t *pml4, uint64_t virtual_addr, uint64_t physical_addr,
                  uint64_t flags) {
  spinlock_acquire(&vmm_lock);
  bool success = false;

  // Determine the indices for each page table level
  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index = (virtual_addr >> 21) & 0x1FF;
  size_t pt_index = (virtual_addr >> 12) & 0x1FF;

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
  // Propagate flags AFTER get_next_level has ensured the entry exists
  pml4_virt[pml4_index] |= propagate_flags;

  // Get PD (or create if missing)
  uint64_t *pd_virt = get_next_level(pdpt_virt, pdpt_index, true);
  if (!pd_virt) {
    klog_puts("[VMM] Error: Failed to get/create PD for vaddr 0x");
    klog_uint64(virtual_addr);
    klog_puts("\n");
    goto unlock;
  }
  pdpt_virt[pdpt_index] |= propagate_flags;

  // Get PT (or create if missing)
  uint64_t *pt_virt = get_next_level(pd_virt, pd_index, true);
  if (!pt_virt) {
    klog_puts("[VMM] Error: Failed to get/create PT for vaddr 0x");
    klog_uint64(virtual_addr);
    klog_puts("\n");
    goto unlock;
  }
  pd_virt[pd_index] |= propagate_flags;

  // Set the page entry
  pt_virt[pt_index] = (physical_addr & PAGE_MASK) | flags | PAGE_FLAG_PRESENT;
  vmm_flush_tlb(virtual_addr);
  success = true;

unlock:
  spinlock_release(&vmm_lock);
  return success;
}

bool vmm_map_huge_page(uint64_t *pml4, uint64_t virtual_addr,
                       uint64_t physical_addr, uint64_t flags) {
  spinlock_acquire(&vmm_lock);
  bool success = false;

  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index = (virtual_addr >> 21) & 0x1FF;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4);
  uint64_t propagate_flags = flags & (PAGE_FLAG_USER | PAGE_FLAG_RW);

  uint64_t *pdpt_virt = get_next_level(pml4_virt, pml4_index, true);
  if (!pdpt_virt)
    goto unlock;
  pml4_virt[pml4_index] |= propagate_flags;

  uint64_t *pd_virt = get_next_level(pdpt_virt, pdpt_index, true);
  if (!pd_virt)
    goto unlock;
  pdpt_virt[pdpt_index] |= propagate_flags;

  // Set the 2MB huge page entry (PS flag)
  pd_virt[pd_index] =
      (physical_addr & PAGE_MASK) | flags | PAGE_FLAG_PRESENT | PAGE_FLAG_PS;
  vmm_flush_tlb(virtual_addr);
  success = true;

unlock:
  spinlock_release(&vmm_lock);
  return success;
}

bool vmm_map_range(uint64_t *pml4, uint64_t virtual_addr,
                   uint64_t physical_addr, size_t pages, uint64_t flags) {
  for (size_t i = 0; i < pages; i++) {
    if (!vmm_map_page(pml4, virtual_addr + (i * 4096),
                      physical_addr + (i * 4096), flags)) {
      return false;
    }
  }
  return true;
}

void vmm_free_empty_tables(uint64_t *pml4, uint64_t virtual_addr) {
  // Currently only called under vmm_lock
  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index = (virtual_addr >> 21) & 0x1FF;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4);
  if (!(pml4_virt[pml4_index] & PAGE_FLAG_PRESENT))
    return;

  uint64_t pdpt_phys = pml4_virt[pml4_index] & PAGE_MASK;
  uint64_t *pdpt_virt = (uint64_t *)PHYS_TO_VIRT(pdpt_phys);
  if (!(pdpt_virt[pdpt_index] & PAGE_FLAG_PRESENT) ||
      (pdpt_virt[pdpt_index] & PAGE_FLAG_PS))
    return;

  uint64_t pd_phys = pdpt_virt[pdpt_index] & PAGE_MASK;
  uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(pd_phys);
  if (!(pd_virt[pd_index] & PAGE_FLAG_PRESENT) ||
      (pd_virt[pd_index] & PAGE_FLAG_PS))
    return;

  uint64_t pt_phys = pd_virt[pd_index] & PAGE_MASK;
  uint64_t *pt_virt = (uint64_t *)PHYS_TO_VIRT(pt_phys);

  // Check if PT is empty
  bool pt_empty = true;
  for (int i = 0; i < 512; i++) {
    if (pt_virt[i] & PAGE_FLAG_PRESENT) {
      pt_empty = false;
      break;
    }
  }

  if (pt_empty) {
    pmm_free_page((void *)pt_phys);
    pd_virt[pd_index] = 0;

    // Check if PD is empty
    bool pd_empty = true;
    for (int i = 0; i < 512; i++) {
      if (pd_virt[i] & PAGE_FLAG_PRESENT) {
        pd_empty = false;
        break;
      }
    }

    if (pd_empty) {
      pmm_free_page((void *)pd_phys);
      pdpt_virt[pdpt_index] = 0;

      // Check if PDPT is empty
      bool pdpt_empty = true;
      for (int i = 0; i < 512; i++) {
        if (pdpt_virt[i] & PAGE_FLAG_PRESENT) {
          pdpt_empty = false;
          break;
        }
      }

      if (pdpt_empty) {
        pmm_free_page((void *)pdpt_phys);
        pml4_virt[pml4_index] = 0;
      }
    }
  }
}

void vmm_unmap_page(uint64_t *pml4, uint64_t virtual_addr) {
  spinlock_acquire(&vmm_lock);

  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index = (virtual_addr >> 21) & 0x1FF;
  size_t pt_index = (virtual_addr >> 12) & 0x1FF;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4);

  // Level 4 -> Level 3
  if (!(pml4_virt[pml4_index] & PAGE_FLAG_PRESENT))
    goto unlock;
  uint64_t *pdpt_virt =
      (uint64_t *)PHYS_TO_VIRT(pml4_virt[pml4_index] & PAGE_MASK);

  // Level 3 -> Level 2
  uint64_t pdpt_entry = pdpt_virt[pdpt_index];
  if (!(pdpt_entry & PAGE_FLAG_PRESENT))
    goto unlock;
  if (pdpt_entry & PAGE_FLAG_PS) {
    // It's a 1GB page. Unmapping it at a 4KB granularity isn't supported here.
    // For safety, we just leave it.
    goto unlock;
  }
  uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(pdpt_entry & PAGE_MASK);

  // Level 2 -> Level 1
  uint64_t pd_entry = pd_virt[pd_index];
  if (!(pd_entry & PAGE_FLAG_PRESENT))
    goto unlock;
  if (pd_entry & PAGE_FLAG_PS) {
    // It's a 2MB page.
    goto unlock;
  }
  uint64_t *pt_virt = (uint64_t *)PHYS_TO_VIRT(pd_entry & PAGE_MASK);

  // Now we're at the leaf PTE
  pt_virt[pt_index] = 0;
  vmm_flush_tlb(virtual_addr);

  // Recurse upward to free empty tables
  vmm_free_empty_tables(pml4, virtual_addr);

unlock:
  spinlock_release(&vmm_lock);
}

// Recursively walks the page structure starting at phys and marks every
// physical page used as a table in the PMM bitmap.
static void vmm_protect_table_recursive(uint64_t phys, int level) {
  if (level < 1)
    return;
  pmm_mark_used((void *)phys, 1);

  uint64_t *virt = (uint64_t *)PHYS_TO_VIRT(phys);
  for (int i = 0; i < 512; i++) {
    uint64_t entry = virt[i];
    if ((entry & PAGE_FLAG_PRESENT) && !(entry & PAGE_FLAG_PS) && level > 1) {
      vmm_protect_table_recursive(entry & PAGE_MASK, level - 1);
    }
  }
}

void vmm_protect_active_tables(void) {
  uint64_t *pml4 = vmm_get_active_pml4();
  // We use level 4 for PML4 itself.
  vmm_protect_table_recursive((uint64_t)pml4, 4);
}

uint64_t vmm_virt_to_phys(uint64_t *pml4_phys, uint64_t virtual_addr) {
  size_t pml4_index = (virtual_addr >> 39) & 0x1FF;
  size_t pdpt_index = (virtual_addr >> 30) & 0x1FF;
  size_t pd_index = (virtual_addr >> 21) & 0x1FF;
  size_t pt_index = (virtual_addr >> 12) & 0x1FF;

  uint64_t *pml4_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)pml4_phys);
  uint64_t entry;

  // Level 4 (PML4)
  entry = pml4_virt[pml4_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return 0;
  // PML4 entries should never be PS=1 in 4-level paging.

  // Level 3 (PDPT)
  uint64_t *pdpt_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pdpt_virt[pdpt_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return 0;
  if (entry & PAGE_FLAG_PS) { // 1GB huge page
    return (entry & 0xFFFFFC0000000ULL) | (virtual_addr & 0x3FFFFFFFULL);
  }

  // Level 2 (PD)
  uint64_t *pd_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pd_virt[pd_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return 0;
  if (entry & PAGE_FLAG_PS) { // 2MB huge page
    return (entry & 0xFFFFFFFE00000ULL) | (virtual_addr & 0x1FFFFFULL);
  }

  // Level 1 (PT)
  uint64_t *pt_virt = (uint64_t *)PHYS_TO_VIRT(entry & PAGE_MASK);
  entry = pt_virt[pt_index];
  if (!(entry & PAGE_FLAG_PRESENT))
    return 0;
  return (entry & PAGE_MASK) | (virtual_addr & 0xFFFULL);
}

// ── Deep-copy helper for page table cloning ─────────────────────────────────
// Recursively clone page table levels.  At level 1 (leaf PT) we allocate
// fresh physical pages and copy their content.  At higher levels we allocate
// new table pages and recurse.
//
// level: 4 = PML4, 3 = PDPT, 2 = PD, 1 = PT
// start / end: range of entries to process (0-511 for full clone)
static uint64_t *clone_table(uint64_t *src_table_phys, int level, size_t start,
                             size_t end) {
  void *new_table_phys = pmm_alloc();
  if (!new_table_phys)
    return NULL;

  uint64_t *new_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_table_phys);
  uint64_t *src_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)src_table_phys);

  // Zero the new table first
  for (size_t i = 0; i < 512; i++) {
    new_virt[i] = 0;
  }

  for (size_t i = start; i < end; i++) {
    if (!(src_virt[i] & PAGE_FLAG_PRESENT))
      continue;
    if (!(src_virt[i] & PAGE_FLAG_USER))
      continue; // Ignore kernel/Limine mappings

    if (level == 1) {
      // Leaf level: allocate a fresh physical page and copy content
      void *new_page_phys = pmm_alloc();
      if (!new_page_phys)
        return NULL; // OOM

      uint8_t *dst = (uint8_t *)PHYS_TO_VIRT((uint64_t)new_page_phys);
      uint8_t *src = (uint8_t *)PHYS_TO_VIRT(src_virt[i] & PAGE_MASK);

      // Copy page content using 64-bit words for speed
      uint64_t *dst64 = (uint64_t *)dst;
      uint64_t *src64 = (uint64_t *)src;
      for (size_t w = 0; w < 512; w++) {
        dst64[w] = src64[w];
      }

      // Preserve flags from original PTE
      new_virt[i] =
          ((uint64_t)new_page_phys & PAGE_MASK) | (src_virt[i] & ~PAGE_MASK);
    } else {
      // Intermediate level: recurse
      uint64_t *child_src_phys = (uint64_t *)(src_virt[i] & PAGE_MASK);
      uint64_t *child_new_phys = clone_table(child_src_phys, level - 1, 0, 512);
      if (!child_new_phys)
        return NULL; // OOM

      // Preserve flags from original entry
      new_virt[i] =
          ((uint64_t)child_new_phys & PAGE_MASK) | (src_virt[i] & ~PAGE_MASK);
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
    if (!(src_pml4_virt[i] & PAGE_FLAG_PRESENT))
      continue;
    if (!(src_pml4_virt[i] & PAGE_FLAG_USER))
      continue; // Ignore kernel/Limine mappings

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

// Helper to check if a virtual address is in a shared VMA
static bool is_shared_vma(struct vma_list *vmas, uint64_t vaddr) {
  if (!vmas)
    return false;
  struct vma *vma = vma_find(vmas, vaddr);
  if (vma && (vma->flags & MAP_SHARED)) {
    return true;
  }
  return false;
}

// Clone table with VMA awareness - shared pages are not copied
static uint64_t *clone_table_vma(uint64_t *src_table_phys, int level,
                                 size_t start, size_t end,
                                 struct vma_list *vmas, uint64_t base_addr) {
  void *new_table_phys = pmm_alloc();
  if (!new_table_phys)
    return NULL;

  uint64_t *new_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)new_table_phys);
  uint64_t *src_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)src_table_phys);

  // Zero the new table first
  for (size_t i = 0; i < 512; i++) {
    new_virt[i] = 0;
  }

  for (size_t i = start; i < end; i++) {
    if (!(src_virt[i] & PAGE_FLAG_PRESENT))
      continue;
    if (!(src_virt[i] & PAGE_FLAG_USER))
      continue; // Ignore kernel mappings

    if (level == 1) {
      // Leaf level: calculate the virtual address for this page
      uint64_t page_vaddr = base_addr | (i << 12);

      // Check if this page is in a shared VMA
      if (is_shared_vma(vmas, page_vaddr)) {
        // Shared mapping: just copy the PTE (share the physical page)
        new_virt[i] = src_virt[i];
      } else {
        // Private mapping: allocate a fresh physical page and copy content
        void *new_page_phys = pmm_alloc();
        if (!new_page_phys)
          return NULL; // OOM

        uint8_t *dst = (uint8_t *)PHYS_TO_VIRT((uint64_t)new_page_phys);
        uint8_t *src = (uint8_t *)PHYS_TO_VIRT(src_virt[i] & PAGE_MASK);

        // Copy page content using 64-bit words for speed
        uint64_t *dst64 = (uint64_t *)dst;
        uint64_t *src64 = (uint64_t *)src;
        for (size_t w = 0; w < 512; w++) {
          dst64[w] = src64[w];
        }

        // Preserve flags from original PTE
        new_virt[i] =
            ((uint64_t)new_page_phys & PAGE_MASK) | (src_virt[i] & ~PAGE_MASK);
      }
    } else {
      // Intermediate level: calculate base address for recursion
      uint64_t child_base = base_addr;
      int shift = 12 + 9 * (level - 1);
      child_base |= ((uint64_t)i << shift);

      // Recurse
      uint64_t *child_src_phys = (uint64_t *)(src_virt[i] & PAGE_MASK);
      uint64_t *child_new_phys =
          clone_table_vma(child_src_phys, level - 1, 0, 512, vmas, child_base);
      if (!child_new_phys)
        return NULL; // OOM

      // Preserve flags from original entry
      new_virt[i] =
          ((uint64_t)child_new_phys & PAGE_MASK) | (src_virt[i] & ~PAGE_MASK);
    }
  }

  return (uint64_t *)new_table_phys; // Return physical address
}

uint64_t vmm_clone_user_mappings_vma(uint64_t *src_pml4_phys,
                                     struct vma_list *vmas) {
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

  // Clone user-space entries (0-255) with VMA awareness
  for (size_t i = 0; i < 256; i++) {
    if (!(src_pml4_virt[i] & PAGE_FLAG_PRESENT))
      continue;
    if (!(src_pml4_virt[i] & PAGE_FLAG_USER))
      continue; // Ignore kernel mappings

    // Calculate base virtual address for this PML4 entry
    uint64_t base_addr = (uint64_t)i << 39;
    // Sign-extend if bit 47 is set (canonical address)
    if (base_addr & (1ULL << 47)) {
      base_addr |= 0xFFFF000000000000ULL;
    }

    uint64_t *child_src_phys = (uint64_t *)(src_pml4_virt[i] & PAGE_MASK);
    uint64_t *child_new_phys =
        clone_table_vma(child_src_phys, 3, 0, 512, vmas, base_addr);
    if (!child_new_phys) {
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

// ── Free all user-space pages and page tables for a given CR3 ───────────────
// Walks PML4 entries 0-255 (user half), frees all mapped physical pages
// and all intermediate page table pages, then frees the PML4 itself.
// CRITICAL: Uses PAGE_MASK to strip NX/available bits from PTEs.
void vmm_free_user_pages(uint64_t cr3) {
  if (cr3 == 0)
    return;

  // Safety: never free the active PML4
  uint64_t active_cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(active_cr3));
  if (cr3 == (active_cr3 & PAGE_MASK)) {
    klog_puts("[VMM] WARNING: refusing to free active CR3!\n");
    return;
  }

  uint64_t hhdm = pmm_get_hhdm_offset();
  uint64_t *pml4_virt = (uint64_t *)(hhdm + cr3);

  for (size_t i = 0; i < 256; i++) {
    if (!(pml4_virt[i] & PAGE_FLAG_PRESENT))
      continue;

    uint64_t pdpt_phys = pml4_virt[i] & PAGE_MASK;
    uint64_t *pdpt_virt = (uint64_t *)(hhdm + pdpt_phys);

    for (size_t j = 0; j < 512; j++) {
      if (!(pdpt_virt[j] & PAGE_FLAG_PRESENT))
        continue;

      // 1GB huge page — skip freeing individual pages
      if (pdpt_virt[j] & PAGE_FLAG_PS)
        continue;

      uint64_t pd_phys = pdpt_virt[j] & PAGE_MASK;
      uint64_t *pd_virt = (uint64_t *)(hhdm + pd_phys);

      for (size_t k = 0; k < 512; k++) {
        if (!(pd_virt[k] & PAGE_FLAG_PRESENT))
          continue;

        // 2MB huge page
        if (pd_virt[k] & PAGE_FLAG_PS) {
          uint64_t huge_phys = pd_virt[k] & PAGE_MASK;
          for (size_t p = 0; p < 512; p++) {
            pmm_free_page((void *)(huge_phys + p * 4096));
          }
          pd_virt[k] = 0;
          continue;
        }

        uint64_t pt_phys = pd_virt[k] & PAGE_MASK;
        uint64_t *pt_virt = (uint64_t *)(hhdm + pt_phys);

        for (size_t l = 0; l < 512; l++) {
          if (pt_virt[l] & PAGE_FLAG_PRESENT) {
            uint64_t page_phys = pt_virt[l] & PAGE_MASK;
            pmm_free_page((void *)page_phys);
          }
        }
        pmm_free_page((void *)pt_phys);
      }
      pmm_free_page((void *)pd_phys);
    }
    pmm_free_page((void *)pdpt_phys);
  }

  // Free the PML4 page itself
  pmm_free_page((void *)cr3);
}

#include "../sched/sched.h"

int vmm_handle_page_fault(uint64_t cr2, uint64_t error_code,
                          struct registers *regs) {
  (void)regs;
  bool user_mode = (error_code & 0x4) != 0;
  bool write_fault = (error_code & 0x2) != 0;

  struct thread *current = sched_get_current();
  if (!current) {
    return -1; // Kernel fault, no process context
  }

  uint64_t target_cr3 = current->cr3;
  if (target_cr3 == 0) {
    // Kernel thread inheritance fallback
    __asm__ volatile("mov %%cr3, %0" : "=r"(target_cr3));
    target_cr3 &= 0xFFFFFFFFFFFFF000ULL;
  }

  // Special case for thread stack expansion (temporary hack until VMA strictly
  // maps stacks)
  if (cr2 >= current->stack_base - 0x100000 &&
      cr2 < current->stack_base + current->stack_size) {
    // Authentically map the missing stack page to resolve the fault and
    // prevent infinite recursion.
    void *phys = pmm_alloc();
    if (phys) {
      vmm_map_page((uint64_t *)current->cr3, cr2 & PAGE_MASK, (uint64_t)phys,
                   PAGE_FLAG_USER | PAGE_FLAG_RW | PAGE_FLAG_PRESENT);
      return 0; // Success: CPU will now re-execute and find the page
    }
  } else {
    // Real VMA validation
    struct vma *vma = vma_find(&current->vmas, cr2);
    if (!vma) {
      if (user_mode) {
        return -1; // Let ISR handle SIGSEGV reporting
      }
      return -1;
    }

    // Validate permissions
    if (write_fault &&
        !(vma->prot &
          0x2)) { // PROT_WRITE mapped to 2 loosely. Assuming sys_mm.c maps this
      if (user_mode) {
        klog_puts("[VMM] User space write violation (SIGSEGV) at 0x");
        klog_uint64(cr2);
        klog_puts("\n");
        sched_terminate_thread(current->tid);
        return 0; // Handled
      }
      return -1;
    }
  }

  // Allocate frame (Zero-Fill Engine)
  void *frame = pmm_alloc_page();
  if (!frame) {
    klog_puts("[VMM] OOM during demand paging!\n");
    if (user_mode) {
      sched_terminate_thread(current->tid);
      return 0;
    }
    return -1;
  }

  uint64_t *frame_virt = (uint64_t *)PHYS_TO_VIRT((uint64_t)frame);
  for (int i = 0; i < 512; i++)
    frame_virt[i] = 0; // Zero Out

  uint64_t flags = PAGE_FLAG_USER | PAGE_FLAG_PRESENT | PAGE_FLAG_RW;

  if (!vmm_map_page((uint64_t *)target_cr3, cr2 & ~0xFFFULL, (uint64_t)frame,
                    flags)) {
    pmm_free_page(frame);
    klog_puts("[VMM] Fatal PT alloc failure in paging engine\n");
    if (user_mode) {
      sched_terminate_thread(current->tid);
      return 0;
    }
    return -1;
  }

  return 0; // successfully handled!
}
void vmm_map_signal_trampoline(uint64_t *pml4) {
  extern uint64_t signal_trampoline_phys;
  if (signal_trampoline_phys == 0)
    return;
  // Map the trampoline page as USER accessible and executable (no NX)
  vmm_map_page(pml4, 0x00007FFFFFFFF000ULL, signal_trampoline_phys,
               PAGE_FLAG_USER);
}
