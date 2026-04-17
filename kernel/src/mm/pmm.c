#include "mm/pmm.h"
#include "console/klog.h"
#include "lock/spinlock.h"
#include "lib/list.h"
#include <stdint.h>

#define MAX_ORDER 20

struct buddy_zone {
    struct list_head free_list[MAX_ORDER];
    spinlock_t lock;
};

struct buddy_block {
    struct list_head node;
    size_t order;
};

static struct buddy_zone b_zone;
static spinlock_t pmm_lock = SPINLOCK_INIT;

static uint8_t *bitmap = NULL;
static size_t bitmap_size = 0; // in bytes
static uint64_t highest_page = 0;
static uint64_t usable_memory = 0;
static uint64_t total_memory = 0;
static uint64_t physical_memory_offset = 0;
static struct limine_memmap_response *internal_memmap = NULL;

static inline void bitmap_set(size_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static inline void bitmap_clear(size_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static inline int bitmap_test(size_t bit) {
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

static inline struct buddy_block *virt_to_buddy(uint64_t phys) {
    return (struct buddy_block *)(phys + physical_memory_offset);
}

static inline uint64_t buddy_to_phys(struct buddy_block *block) {
    return (uint64_t)block - physical_memory_offset;
}

static inline size_t get_order(size_t count) {
    size_t order = 0;
    size_t size = 1;
    while (size < count) {
        size *= 2;
        order++;
    }
    return order;
}

size_t pmm_get_free_pages(void) {
    size_t free_pages = 0;
    spinlock_acquire(&b_zone.lock);
    for (int order = 0; order < MAX_ORDER; order++) {
        struct list_head *pos;
        list_for_each(pos, &b_zone.free_list[order]) {
            free_pages += (1ULL << order);
        }
    }
    spinlock_release(&b_zone.lock);
    return free_pages;
}

// Internal function to add a free block to the buddy system
static void buddy_free_internal(uint64_t phys, size_t order) {
    uint64_t pfn = phys / PAGE_SIZE;

    // Clear bitmap for this block
    for (size_t i = 0; i < (1ULL << order); i++) {
        if (pfn + i < highest_page) {
            bitmap_clear(pfn + i);
        }
    }

    while (order < MAX_ORDER - 1) {
        uint64_t buddy_pfn = pfn ^ (1ULL << order);
        
        bool buddy_free = true;
        if (buddy_pfn >= highest_page || bitmap_test(buddy_pfn)) {
            buddy_free = false;
        }
        if (!buddy_free) break;

        struct buddy_block *buddy = virt_to_buddy(buddy_pfn * PAGE_SIZE);
        if (buddy->order == order) {
            // It's in the same order list, coalesce
            list_del(&buddy->node);
            pfn = (pfn < buddy_pfn) ? pfn : buddy_pfn;
            order++;
        } else {
            // Free but split into smaller blocks; wait for them to coalesce up
            break;
        }
    }

    struct buddy_block *block = virt_to_buddy(pfn * PAGE_SIZE);
    block->order = order;
    list_add_tail(&block->node, &b_zone.free_list[order]);
}

void pmm_init(struct limine_memmap_response *memmap, uint64_t hhdm_offset) {
    physical_memory_offset = hhdm_offset;
    internal_memmap = memmap;
    highest_page = 0;

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

        uint64_t top = entry->base + entry->length;
        if (top > highest_page * PAGE_SIZE) {
            highest_page = top / PAGE_SIZE;
        }
    }

    bitmap_size = highest_page / 8;
    if (highest_page % 8 != 0)
        bitmap_size++;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= bitmap_size) {
            bitmap = (uint8_t *)(entry->base + hhdm_offset);
            for (size_t b = 0; b < bitmap_size; b++) {
                bitmap[b] = 0xFF; // Mark all used initially
            }
            break;
        }
    }

    uint64_t bitmap_phys_base = (uint64_t)bitmap - hhdm_offset;

    for (int i = 0; i < MAX_ORDER; i++) {
        INIT_LIST_HEAD(&b_zone.free_list[i]);
    }

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            uint64_t entry_phys_base = entry->base;
            uint64_t entry_length = entry->length;
            
            uint64_t p = 0;
            while (p < entry_length) {
                uint64_t phys = entry_phys_base + p;
                uint64_t pfn = phys / PAGE_SIZE;
                
                // Skip first 1MB
                if (phys < 0x100000) {
                    p += PAGE_SIZE;
                    continue;
                }
                
                // Skip bitmap region
                if (phys >= bitmap_phys_base && phys < bitmap_phys_base + bitmap_size) {
                    p += PAGE_SIZE;
                    continue;
                }

                // Determine the largest order we can use here.
                // It must be:
                // 1. Power of two pages
                // 2. Aligned to that power of two
                // 3. Not exceeding MAX_ORDER - 1
                // 4. Not overlapping with bitmap or 1MB reserve
                // 5. Fit within the remaining entry length
                
                size_t order = 0;
                while (order < MAX_ORDER - 1) {
                    uint64_t next_order_pages = 1ULL << (order + 1);
                    uint64_t next_order_size = next_order_pages * PAGE_SIZE;
                    
                    if (p + next_order_size > entry_length) break;
                    if ((phys % next_order_size) != 0) break;
                    
                    // Check if next order would overlap with bitmap
                    uint64_t phys_end = phys + next_order_size;
                    if (!(phys_end <= bitmap_phys_base || phys >= bitmap_phys_base + bitmap_size)) {
                        break;
                    }

                    order++;
                }

                buddy_free_internal(phys, order);
                usable_memory += (1ULL << order) * PAGE_SIZE;
                p += (1ULL << order) * PAGE_SIZE;
            }
        }
    }
}

void *pmm_alloc_pages(size_t count) {
    if (count == 0) return NULL;

    size_t order = get_order(count);
    if (order >= MAX_ORDER) return NULL;

    spinlock_acquire(&b_zone.lock);

    size_t cur_order = order;
    while (cur_order < MAX_ORDER && list_empty(&b_zone.free_list[cur_order])) {
        cur_order++;
    }

    if (cur_order == MAX_ORDER) {
        spinlock_release(&b_zone.lock);
        return NULL; // OOM
    }

    struct buddy_block *block = list_first_entry(&b_zone.free_list[cur_order], struct buddy_block, node);
    list_del(&block->node);

    uint64_t pfn = buddy_to_phys(block) / PAGE_SIZE;

    // Split down to requested order
    while (cur_order > order) {
        cur_order--;
        uint64_t buddy_pfn = pfn + (1ULL << cur_order);
        struct buddy_block *buddy = virt_to_buddy(buddy_pfn * PAGE_SIZE);
        buddy->order = cur_order;
        list_add_tail(&buddy->node, &b_zone.free_list[cur_order]);
    }

    // Mark as used in bitmap
    size_t actual_pages = 1ULL << order;
    for (size_t i = 0; i < actual_pages; i++) {
        if (pfn + i < highest_page) {
            bitmap_set(pfn + i);
        }
    }

    spinlock_release(&b_zone.lock);
    return (void *)(pfn * PAGE_SIZE);
}

void *pmm_alloc_pages_constrained(size_t count, uint64_t max_phys_addr) {
    if (count == 0) return NULL;

    size_t order = get_order(count);
    if (order >= MAX_ORDER) return NULL;

    spinlock_acquire(&b_zone.lock);

    for (size_t cur_order = order; cur_order < MAX_ORDER; cur_order++) {
        struct buddy_block *found_block = NULL;
        struct list_head *pos;
        
        list_for_each(pos, &b_zone.free_list[cur_order]) {
            struct buddy_block *block = list_entry(pos, struct buddy_block, node);
            uint64_t phys = buddy_to_phys(block);
            if (phys + (1ULL << cur_order) * PAGE_SIZE <= max_phys_addr) {
                found_block = block;
                break;
            }
        }

        if (found_block) {
            list_del(&found_block->node);
            uint64_t pfn = buddy_to_phys(found_block) / PAGE_SIZE;

            // Split down to requested order
            while (cur_order > order) {
                cur_order--;
                uint64_t buddy_pfn = pfn + (1ULL << cur_order);
                struct buddy_block *buddy = virt_to_buddy(buddy_pfn * PAGE_SIZE);
                buddy->order = cur_order;
                list_add_tail(&buddy->node, &b_zone.free_list[cur_order]);
            }

            // Mark as used in bitmap
            size_t actual_pages = 1ULL << order;
            for (size_t i = 0; i < actual_pages; i++) {
                if (pfn + i < highest_page) {
                    bitmap_set(pfn + i);
                }
            }

            spinlock_release(&b_zone.lock);
            return (void *)(pfn * PAGE_SIZE);
        }
    }

    spinlock_release(&b_zone.lock);
    return NULL; // No block found within constraints
}

void *pmm_alloc_page(void) {
    return pmm_alloc_pages(1);
}

void pmm_free_pages(void *ptr, size_t count) {
    if (!ptr || count == 0) return;

    size_t order = get_order(count);
    if (order >= MAX_ORDER) return;

    uint64_t addr = (uint64_t)ptr;
    if ((addr & (PAGE_SIZE - 1)) != 0) {
        klog_puts("[PMM] Warning: ignoring unaligned free addr=");
        klog_uint64(addr);
        klog_puts("\n");
        return;
    }

    // Safety check for active PML4
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t pml4_phys = cr3 & 0x000FFFFFFFFFF000ULL;
    if (addr == pml4_phys) {
        klog_puts("[PMM] CRITICAL ERROR: Attempted to free active PML4 page table!\n");
        return;
    }

    spinlock_acquire(&b_zone.lock);
    buddy_free_internal(addr, order);
    spinlock_release(&b_zone.lock);
}

void pmm_free_page(void *ptr) {
    pmm_free_pages(ptr, 1);
}

void pmm_mark_used(void *ptr, size_t count) {
    if (!ptr || count == 0) return;
    uint64_t addr = (uint64_t)ptr;
    size_t start_bit = addr / PAGE_SIZE;

    // This is primarily an early boot mechanism and won't safely remove
    // memory that is already actively being managed by Buddy Lists.
    for (size_t i = start_bit; i < start_bit + count; i++) {
        if (i < highest_page) {
            bitmap_set(i);
        }
    }
}

void pmm_reclaim_bootloader(void) {
    if (!internal_memmap) return;

    extern void vmm_protect_active_tables(void);

    // Allocate a temporary bitmap to track which pages are reclaimable.
    // pmm_alloc_pages returns a physical address.
    size_t temp_count = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t order = get_order(temp_count);
    uint8_t *temp_bitmap_phys = pmm_alloc_pages(temp_count);
    if (!temp_bitmap_phys) {
        klog_puts("[PMM] FATAL: Failed to allocate temp bitmap for reclaim. count=");
        klog_uint64(temp_count);
        klog_puts(" order=");
        klog_uint64(order);
        klog_puts(" bitmap_size=");
        klog_uint64(bitmap_size);
        klog_puts("\n");
        return;
    }
    uint8_t *temp_bitmap = (uint8_t *)((uint64_t)temp_bitmap_phys + physical_memory_offset);
    
    for (size_t i = 0; i < bitmap_size; i++) {
        temp_bitmap[i] = 0;
    }

    // Step 1: Mark all reclaimable regions in the temp bitmap, and clear them in the main bitmap.
    for (uint64_t i = 0; i < internal_memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = internal_memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            for (uint64_t p = 0; p < entry->length; p += PAGE_SIZE) {
                uint64_t phys = entry->base + p;
                if (phys < 0x100000) continue; 
                
                uint64_t pfn = phys / PAGE_SIZE;
                temp_bitmap[pfn / 8] |= (1 << (pfn % 8)); // Mark in temp
                bitmap_clear(pfn);                        // Clear in main
            }
        }
    }

    // Step 2: VMM sets main bitmap to 1 for active tables via pmm_mark_used
    vmm_protect_active_tables();

    spinlock_acquire(&b_zone.lock);
    
    // Step 3: Iterate through temp_bitmap. 
    // If a page is meant to be reclaimed (in temp) but wasn't protected by VMM (main == 0),
    // we MUST temporarily set it back to 1 in the main bitmap to prevent false coalescing when processing other pages.
    // If it WAS protected (main == 1), we remove it from temp_bitmap so we don't free it.
    for (size_t b = 0; b < bitmap_size; b++) {
        if (temp_bitmap[b]) {
            for (int i = 0; i < 8; i++) {
                if (temp_bitmap[b] & (1 << i)) {
                    uint64_t pfn = b * 8 + i;
                    if (!bitmap_test(pfn)) {
                        bitmap_set(pfn); // Prevent uninitialized coalescing
                    } else {
                        temp_bitmap[b] &= ~(1 << i); // Protected, don't free later
                    }
                }
            }
        }
    }

    // Step 4: Now safely pass exactly the unprotected pages to the buddy allocator in large blocks
    for (size_t b = 0; b < bitmap_size; b++) {
        if (temp_bitmap[b]) {
            for (int i = 0; i < 8; i++) {
                if (temp_bitmap[b] & (1 << i)) {
                    uint64_t start_pfn = (uint64_t)b * 8 + i;
                    
                    // Find length of contiguous run of set bits in temp_bitmap
                    size_t count = 0;
                    uint64_t curr_pfn = start_pfn;
                    while (curr_pfn < highest_page) {
                        size_t curr_b = curr_pfn / 8;
                        int curr_bit_idx = curr_pfn % 8;
                        if (temp_bitmap[curr_b] & (1 << curr_bit_idx)) {
                            count++;
                            // Clear bit so we don't process it again in the outer loop
                            temp_bitmap[curr_b] &= ~(1 << curr_bit_idx);
                            curr_pfn++;
                        } else {
                            break;
                        }
                    }

                    // Add this run in the largest possible blocks
                    uint64_t p = 0;
                    while (p < count) {
                        uint64_t phys = (start_pfn + p) * PAGE_SIZE;
                        size_t remaining = count - p;
                        
                        size_t order = 0;
                        while (order < MAX_ORDER - 1) {
                            uint64_t next_order_pages = 1ULL << (order + 1);
                            if (next_order_pages > remaining) break;
                            if ((phys % (next_order_pages * PAGE_SIZE)) != 0) break;
                            order++;
                        }
                        
                        buddy_free_internal(phys, order);
                        usable_memory += (1ULL << order) * PAGE_SIZE;
                        p += (1ULL << order);
                    }
                }
            }
        }
    }

    internal_memmap = NULL;
    spinlock_release(&b_zone.lock);

    // Free the temp bitmap
    pmm_free_pages(temp_bitmap_phys, temp_count);
}

uint64_t pmm_get_usable_memory(void) { return usable_memory; }
uint64_t pmm_get_total_memory(void) { return total_memory; }
uint64_t pmm_get_hhdm_offset(void) { return physical_memory_offset; }
