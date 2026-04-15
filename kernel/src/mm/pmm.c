#include "mm/pmm.h"
#include <stdint.h>
#include "lock/spinlock.h"
#include "console/klog.h"

static spinlock_t pmm_lock = SPINLOCK_INIT;

static uint8_t *bitmap = NULL;
static size_t bitmap_size = 0; // in bytes
static uint64_t highest_page = 0;
static uint64_t usable_memory = 0;
static uint64_t total_memory = 0;
static uint64_t physical_memory_offset = 0;
static struct limine_memmap_response *internal_memmap = NULL;
static size_t last_scanned_page = 0;

// Set bit in bitmap
static inline void bitmap_set(size_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

// Clear bit in bitmap
static inline void bitmap_clear(size_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

// Test bit in bitmap
static inline int bitmap_test(size_t bit) {
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset) {
    physical_memory_offset = hhdm_offset;
    internal_memmap = memmap;
    highest_page = 0;

    // First pass: Calculate total memory, usable memory, and find highest page index.
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE ||
            entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            entry->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES ||
            entry->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE ||
            entry->type == LIMINE_MEMMAP_ACPI_NVS ||
            entry->type == LIMINE_MEMMAP_FRAMEBUFFER) {
            total_memory += entry->length;
        }
        
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            usable_memory += entry->length;
        }
        
        uint64_t top = entry->base + entry->length;
        if (top > highest_page * PAGE_SIZE) {
            highest_page = top / PAGE_SIZE;
        }
    }

    bitmap_size = highest_page / 8;
    if (highest_page % 8 != 0) bitmap_size++;

    // Second pass: Find a usable chunk to store the bitmap itself.
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= bitmap_size) {
            // Found a place. The bitmap lives at this physical address, 
            // but we MUST access it via the HHDM virtual offset!
            bitmap = (uint8_t *)(entry->base + hhdm_offset);
            
            // We must now ensure we don't consider the bitmap's own memory as "free"
            // We will initialize the bitmap as fully USED (all 1s) first.
            for (size_t b = 0; b < bitmap_size; b++) {
                bitmap[b] = 0xFF; // Mark all blocks used
            }
            break;
        }
    }

    // Third pass: Mark usable blocks as FREE (0)
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            for (uint64_t p = 0; p < entry->length; p += PAGE_SIZE) {
                bitmap_clear((entry->base + p) / PAGE_SIZE);
            }
        }
    }

    // Now, explicitly mark the actual bitmap memory area as USED again,
    // so `pmm_alloc` never gives away our bitmap structure!
    uint64_t bitmap_phys_base = (uint64_t)bitmap - hhdm_offset;
    for (uint64_t p = 0; p < bitmap_size; p += PAGE_SIZE) {
        bitmap_set((bitmap_phys_base + p) / PAGE_SIZE);
    }
    
    // Also, mark page 0 as USED to prevent allocating address zero (makes NULL checks safer).
    bitmap_set(0);

    // Keep the first 1 MiB permanently reserved. Firmware/boot-time data and
    // early paging structures may live here, and accidental reuse can corrupt
    // active page tables.
    for (uint64_t p = 0; p < 0x100000; p += PAGE_SIZE) {
        bitmap_set(p / PAGE_SIZE);
    }
}

void *pmm_alloc_blocks(size_t count) {
    if (count == 0) return NULL;
    
    spinlock_acquire(&pmm_lock);
    
    size_t consecutive = 0;
    size_t start_bit = 0;

    for (size_t i = last_scanned_page; i < highest_page; i++) {
        if (!bitmap_test(i)) {
            if (consecutive == 0) start_bit = i;
            consecutive++;
            if (consecutive == count) {
                // Return this chunk! Mark it as used.
                for (size_t j = start_bit; j < start_bit + count; j++) {
                    bitmap_set(j);
                }
                last_scanned_page = start_bit + count;
                spinlock_release(&pmm_lock);
                return (void *)(start_bit * PAGE_SIZE); // Physical ptr
            }
        } else {
            consecutive = 0; // Reset
        }
    }
    
    // If not found, wrap around and check from the beginning!
    if (last_scanned_page > 0) {
        consecutive = 0;
        start_bit = 0;
        for (size_t i = 0; i < last_scanned_page; i++) {
            if (!bitmap_test(i)) {
                if (consecutive == 0) start_bit = i;
                consecutive++;
                if (consecutive == count) {
                    for (size_t j = start_bit; j < start_bit + count; j++) {
                        bitmap_set(j);
                    }
                    last_scanned_page = start_bit + count;
                    spinlock_release(&pmm_lock);
                    return (void *)(start_bit * PAGE_SIZE); 
                }
            } else {
                consecutive = 0; 
            }
        }
    }

    spinlock_release(&pmm_lock);
    return NULL; // Out of memory
}

void *pmm_alloc(void) {
    return pmm_alloc_blocks(1);
}

void pmm_free_blocks(void *ptr, size_t count) {
    if (!ptr || count == 0) return;

    uint64_t addr = (uint64_t)ptr;
    uint64_t max_phys = highest_page * PAGE_SIZE;

    // PMM owns physical page frame indices, so frees must be page-aligned
    // physical addresses inside known RAM range.
    if ((addr & (PAGE_SIZE - 1)) != 0) {
        klog_puts("[PMM] Warning: ignoring unaligned free addr=");
        klog_uint64(addr);
        klog_puts("\n");
        return;
    }
    if (addr >= max_phys) {
        klog_puts("[PMM] Warning: ignoring out-of-range free addr=");
        klog_uint64(addr);
        klog_puts(" max=");
        klog_uint64(max_phys);
        klog_puts("\n");
        return;
    }

    spinlock_acquire(&pmm_lock);
    size_t start_bit = addr / PAGE_SIZE;

    if (start_bit + count > highest_page) {
        count = highest_page - start_bit;
    }

    for (size_t i = start_bit; i < start_bit + count; i++) {
        bitmap_clear(i);
    }
    
    // If we free memory behind the cursor, back up the cursor!
    if (start_bit < last_scanned_page) {
        last_scanned_page = start_bit;
    }
    spinlock_release(&pmm_lock);
}

void pmm_free(void *ptr) {
    pmm_free_blocks(ptr, 1);
}

void pmm_reclaim_bootloader(void) {
    if (!internal_memmap) return;
    
    for (uint64_t i = 0; i < internal_memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = internal_memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            for (uint64_t p = 0; p < entry->length; p += PAGE_SIZE) {
                bitmap_clear((entry->base + p) / PAGE_SIZE);
            }
            usable_memory += entry->length;
        }
    }
    
    // Nullify pointer to prevent duplicate reclamations
    internal_memmap = NULL;
    last_scanned_page = 0; // Reset cursor to allow allocations in freshly freed space
}

uint64_t pmm_get_usable_memory(void) {
    return usable_memory;
}

uint64_t pmm_get_total_memory(void) {
    return total_memory;
}

uint64_t pmm_get_hhdm_offset(void) {
    return physical_memory_offset;
}
