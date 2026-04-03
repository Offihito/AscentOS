#include "mm/heap.h"
#include "mm/pmm.h"
#include "lib/string.h"
#include "console/console.h"

#define HEAP_MAGIC 0xC001CAFE
#define ALIGN_UP(val, align) (((val) + (align) - 1) & ~((align) - 1))

struct heap_segment {
    uint32_t magic;
    bool is_free;
    size_t size;
    struct heap_segment *next;
    struct heap_segment *prev;
} __attribute__((aligned(16)));

static struct heap_segment *head_segment = NULL;

void heap_init(void) {
    head_segment = NULL;
}

void *kmalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    // Align size to 16 bytes for proper memory alignment
    size_t size_aligned = ALIGN_UP(size, 16);
    struct heap_segment *current = head_segment;

    // Search for a suitable free segment
    while (current != NULL) {
        if (current->is_free && current->size >= size_aligned) {
            // Split segment if it's much larger than needed
            if (current->size > size_aligned + sizeof(struct heap_segment) + 16) {
                struct heap_segment *new_segment = (struct heap_segment *)((uint8_t *)current + sizeof(struct heap_segment) + size_aligned);
                new_segment->magic = HEAP_MAGIC;
                new_segment->is_free = true;
                new_segment->size = current->size - size_aligned - sizeof(struct heap_segment);
                new_segment->next = current->next;
                new_segment->prev = current;
                
                if (new_segment->next) {
                    new_segment->next->prev = new_segment;
                }
                
                current->next = new_segment;
                current->size = size_aligned;
            }
            
            current->is_free = false;
            return (void *)((uint8_t *)current + sizeof(struct heap_segment));
        }
        current = current->next;
    }

    // No free segment found, ask PMM for more memory
    size_t total_needed = size_aligned + sizeof(struct heap_segment);
    size_t pages_needed = ALIGN_UP(total_needed, PAGE_SIZE) / PAGE_SIZE;

    void *phys = pmm_alloc_blocks(pages_needed);
    if (!phys) {
        console_puts("[ERR] kmalloc Out of Memory!\n");
        return NULL;
    }

    // Convert physical address to HHDM virtual address
    struct heap_segment *new_seg = (struct heap_segment *)((uint64_t)phys + pmm_get_hhdm_offset());
    new_seg->magic = HEAP_MAGIC;
    new_seg->is_free = false;
    new_seg->size = (pages_needed * PAGE_SIZE) - sizeof(struct heap_segment);

    // Link it to the end of the list
    struct heap_segment *last = head_segment;
    if (!last) {
        head_segment = new_seg;
        new_seg->next = NULL;
        new_seg->prev = NULL;
    } else {
        while (last->next != NULL) {
            last = last->next;
        }
        last->next = new_seg;
        new_seg->prev = last;
        new_seg->next = NULL;
    }

    // Split the newly allocated large block if possible
    if (new_seg->size > size_aligned + sizeof(struct heap_segment) + 16) {
        struct heap_segment *split = (struct heap_segment *)((uint8_t *)new_seg + sizeof(struct heap_segment) + size_aligned);
        split->magic = HEAP_MAGIC;
        split->is_free = true;
        split->size = new_seg->size - size_aligned - sizeof(struct heap_segment);
        split->next = NULL;
        split->prev = new_seg;
        
        new_seg->next = split;
        new_seg->size = size_aligned;
    }

    return (void *)((uint8_t *)new_seg + sizeof(struct heap_segment));
}

void kfree(void *ptr) {
    if (!ptr) {
        return;
    }

    struct heap_segment *seg = (struct heap_segment *)((uint8_t *)ptr - sizeof(struct heap_segment));
    
    // Check for memory corruption or double-free
    if (seg->magic != HEAP_MAGIC) {
        console_puts("[WARN] kfree: Magic number mismatch! Possible memory corruption.\n");
        return;
    }

    if (seg->is_free) {
        console_puts("[WARN] kfree: Double free detected!\n");
        return;
    }

    seg->is_free = true;

    // Coalesce with next segment if it is free
    if (seg->next && seg->next->is_free) {
        seg->size += seg->next->size + sizeof(struct heap_segment);
        seg->next = seg->next->next;
        if (seg->next) {
            seg->next->prev = seg;
        }
    }

    // Coalesce with previous segment if it is free
    if (seg->prev && seg->prev->is_free) {
        seg->prev->size += seg->size + sizeof(struct heap_segment);
        seg->prev->next = seg->next;
        if (seg->next) {
            seg->next->prev = seg->prev;
        }
    }
}

void *kcalloc(size_t num, size_t size) {
    size_t total = num * size;
    void *ptr = kmalloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) {
        return kmalloc(new_size);
    }

    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    struct heap_segment *seg = (struct heap_segment *)((uint8_t *)ptr - sizeof(struct heap_segment));
    
    if (seg->magic != HEAP_MAGIC) {
        return NULL; // Invalid pointer
    }

    // If the current block is already large enough, just return it
    if (seg->size >= new_size) {
        // We could shrink the block here, but for simplicity, we don't.
        return ptr;
    }

    // Allocate a new block
    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) {
        return NULL;
    }

    // Copy old data to the new block
    memcpy(new_ptr, ptr, seg->size);
    
    // Free the old block
    kfree(ptr);
    
    return new_ptr;
}
