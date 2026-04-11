// Virtual Memory Area management
#include "vma.h"
#include "../lib/string.h"

void vma_list_init(struct vma_list *list) {
    memset(list, 0, sizeof(struct vma_list));
    list->count = 0;
    for (int i = 0; i < VMA_MAX_REGIONS; i++) {
        list->regions[i].active = false;
    }
}

int vma_add(struct vma_list *list, uint64_t start, uint64_t end,
            uint64_t prot, uint64_t flags, int fd, uint64_t offset) {
    // Find a free slot
    for (int i = 0; i < VMA_MAX_REGIONS; i++) {
        if (!list->regions[i].active) {
            list->regions[i].start = start;
            list->regions[i].end = end;
            list->regions[i].prot = prot;
            list->regions[i].flags = flags;
            list->regions[i].fd = fd;
            list->regions[i].offset = offset;
            list->regions[i].active = true;
            list->count++;
            return i;
        }
    }
    return -1; // No free slots
}

bool vma_remove(struct vma_list *list, uint64_t start, uint64_t end) {
    bool removed = false;
    
    for (int i = 0; i < VMA_MAX_REGIONS; i++) {
        struct vma *v = &list->regions[i];
        if (!v->active) continue;
        
        // Check for overlap
        if (v->start < end && v->end > start) {
            // Case 1: Complete overlap - remove entire VMA
            if (start <= v->start && end >= v->end) {
                v->active = false;
                list->count--;
                removed = true;
            }
            // Case 2: Unmap from middle - split into two
            else if (start > v->start && end < v->end) {
                // Find a new slot for the second half
                int new_idx = vma_add(list, end, v->end, v->prot, v->flags, v->fd, 
                                      v->offset + (end - v->start));
                if (new_idx >= 0) {
                    v->end = start;
                    removed = true;
                }
            }
            // Case 3: Unmap from start
            else if (start <= v->start) {
                v->start = end;
                removed = true;
            }
            // Case 4: Unmap from end
            else if (end >= v->end) {
                v->end = start;
                removed = true;
            }
        }
    }
    
    return removed;
}

struct vma *vma_find(struct vma_list *list, uint64_t addr) {
    for (int i = 0; i < VMA_MAX_REGIONS; i++) {
        struct vma *v = &list->regions[i];
        if (v->active && addr >= v->start && addr < v->end) {
            return v;
        }
    }
    return NULL;
}

struct vma *vma_find_overlap(struct vma_list *list, uint64_t start, uint64_t end) {
    for (int i = 0; i < VMA_MAX_REGIONS; i++) {
        struct vma *v = &list->regions[i];
        if (v->active && v->start < end && v->end > start) {
            return v;
        }
    }
    return NULL;
}

void vma_list_clone(struct vma_list *dst, struct vma_list *src) {
    vma_list_init(dst);
    
    for (int i = 0; i < VMA_MAX_REGIONS; i++) {
        if (src->regions[i].active) {
            struct vma *src_vma = &src->regions[i];
            // Copy the VMA - shared mappings stay shared, private will be
            // handled by page table cloning
            vma_add(dst, src_vma->start, src_vma->end, src_vma->prot,
                    src_vma->flags, src_vma->fd, src_vma->offset);
        }
    }
}
