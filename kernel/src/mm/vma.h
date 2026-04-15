#ifndef VMA_H
#define VMA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Linux mmap flags (must match sys_mm.c)
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

// Maximum number of VMAs per process
// DOOM requires many mappings for WAD data, sounds, textures, etc.
#define VMA_MAX_REGIONS 1024

// VMA structure - tracks a memory mapping region
struct vma {
    uint64_t start;        // Start virtual address (page-aligned)
    uint64_t end;          // End virtual address (page-aligned, exclusive)
    uint64_t prot;         // Protection flags (PROT_READ, PROT_WRITE, PROT_EXEC)
    uint64_t flags;        // Mapping flags (MAP_SHARED, MAP_PRIVATE, MAP_ANONYMOUS)
    uint64_t offset;       // File offset (for file-backed mappings)
    int fd;                // File descriptor (for file-backed mappings, -1 if anonymous)
    bool active;           // Whether this VMA slot is in use
};

// VMA list for a process
struct vma_list {
    struct vma regions[VMA_MAX_REGIONS];
    int count;             // Number of active regions
};

// Initialize a VMA list
void vma_list_init(struct vma_list *list);

// Add a new VMA region, returns index or -1 on failure
int vma_add(struct vma_list *list, uint64_t start, uint64_t end,
            uint64_t prot, uint64_t flags, int fd, uint64_t offset);

// Remove a VMA region by address range
// Returns true if any region was removed/split
bool vma_remove(struct vma_list *list, uint64_t start, uint64_t end);

// Find VMA containing a given address
struct vma *vma_find(struct vma_list *list, uint64_t addr);

// Find VMA that overlaps with given range
struct vma *vma_find_overlap(struct vma_list *list, uint64_t start, uint64_t end);

// Clone VMA list for fork (shared mappings stay shared, private get copied)
void vma_list_clone(struct vma_list *dst, struct vma_list *src);

#endif // VMA_H
