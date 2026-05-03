#include "dma_alloc.h"
#include "../console/klog.h"
#include "../lib/string.h"
#include "../lock/spinlock.h"
#include "pmm.h"
#include "vmm.h"
#include <stdbool.h>

static spinlock_t dma_lock = SPINLOCK_INIT;

// DMA memory regions - we prefer low memory (1-16MB) for DMA buffers
// to avoid conflicts with MMIO regions and framebuffers at higher addresses
#define DMA_LOW_START 0x100000       // 1MB
#define DMA_LOW_END 0x1000000        // 16MB
#define DMA_32BIT_END 0x100000000ULL // 4GB

void dma_alloc_init(void) { klog_puts("[DMA] Allocator initialized\n"); }

dma_buffer_t *dma_alloc(size_t size, uint32_t flags) {
  if (size == 0)
    return NULL;

  spinlock_acquire(&dma_lock);

  // Calculate number of pages needed
  size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

  // Allocate physical memory
  void *phys = NULL;

  if (flags & DMA_FLAG_LOW) {
    // Try low memory first (1-16MB region)
    phys = pmm_alloc_pages_range(pages, DMA_LOW_START, DMA_LOW_END);
  }

  if (!phys && (flags & DMA_FLAG_32BIT)) {
    // Try below 4GB
    phys = pmm_alloc_pages_range(pages, DMA_LOW_START, DMA_32BIT_END);
  }

  if (!phys) {
    // Default: try low memory, then anywhere below 4GB
    phys = pmm_alloc_pages_range(pages, DMA_LOW_START, DMA_LOW_END);
    if (!phys) {
      phys = pmm_alloc_pages_range(pages, DMA_LOW_END, DMA_32BIT_END);
    }
  }

  if (!phys) {
    spinlock_release(&dma_lock);
    klog_puts("[DMA] Failed to allocate physical memory\n");
    return NULL;
  }

  // Map as uncached by default (unless NOCACHE flag is NOT set)
  uint64_t virt = (uint64_t)phys + pmm_get_hhdm_offset();
  uint64_t *pml4 = vmm_get_active_pml4();

  // Map each page as uncached (PCD|PWT)
  uint64_t map_flags =
      PAGE_FLAG_PRESENT | PAGE_FLAG_RW | PAGE_FLAG_PCD | PAGE_FLAG_PWT;

  for (size_t i = 0; i < pages; i++) {
    uint64_t page_phys = (uint64_t)phys + i * PAGE_SIZE;
    uint64_t page_virt = virt + i * PAGE_SIZE;

    if (!vmm_map_page(pml4, page_virt, page_phys, map_flags)) {
      // Failed to map - free physical memory
      // Note: we should free all pages, but pmm_free_pages expects contiguous
      klog_puts("[DMA] Failed to map page\n");
      spinlock_release(&dma_lock);
      return NULL;
    }
    vmm_flush_tlb(page_virt);
  }

  // Flush cache to ensure no stale data
  __asm__ volatile("wbinvd" ::: "memory");

  // Allocate the dma_buffer_t structure
  // We use kmalloc for this small structure (not DMA memory)
  extern void *kmalloc(size_t);
  dma_buffer_t *buf = (dma_buffer_t *)kmalloc(sizeof(dma_buffer_t));
  if (!buf) {
    // Can't allocate structure - but physical memory is allocated
    // This is a leak, but better than crashing
    klog_puts("[DMA] Failed to allocate buffer structure\n");
    spinlock_release(&dma_lock);
    return NULL;
  }

  buf->virt = (void *)virt;
  buf->phys = (uint64_t)phys;
  buf->size = size;
  buf->pages = pages;

  // Zero the buffer
  memset(buf->virt, 0, pages * PAGE_SIZE);

  spinlock_release(&dma_lock);
  return buf;
}

void dma_free(dma_buffer_t *buf) {
  if (!buf)
    return;

  spinlock_acquire(&dma_lock);

  // Free physical pages
  for (size_t i = 0; i < buf->pages; i++) {
    void *page_phys = (void *)(buf->phys + i * PAGE_SIZE);
    pmm_free_page(page_phys);
  }

  // Free the structure
  extern void kfree(void *);
  kfree(buf);

  spinlock_release(&dma_lock);
}

// Convenience function for single page allocation
void *dma_alloc_page(uint64_t *phys_out) {
  dma_buffer_t *buf = dma_alloc(PAGE_SIZE, DMA_FLAG_32BIT);
  if (!buf)
    return NULL;

  if (phys_out)
    *phys_out = buf->phys;

  void *virt = buf->virt;

  // Free the structure but keep the memory
  extern void kfree(void *);
  kfree(buf);

  return virt;
}

void *dma_alloc_pages(size_t count, uint64_t *phys_out) {
  dma_buffer_t *buf = dma_alloc(count * PAGE_SIZE, DMA_FLAG_32BIT);
  if (!buf)
    return NULL;

  if (phys_out)
    *phys_out = buf->phys;

  void *virt = buf->virt;

  extern void kfree(void *);
  kfree(buf);

  return virt;
}

void dma_free_page(void *virt) {
  if (!virt)
    return;

  // Calculate physical address from virtual
  uint64_t phys = (uint64_t)virt - pmm_get_hhdm_offset();
  pmm_free_page((void *)phys);
}
