#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

// Memory map entry (BIOS E820)
struct memory_map_entry {
    unsigned long base;
    unsigned long length;
    unsigned int  type;
    unsigned int  acpi_extended;
} __attribute__((packed));


void pmm_init(struct memory_map_entry* mmap, unsigned int mmap_count);

// Single-frame allocation / deallocation
void* pmm_alloc_frame(void);
void  pmm_free_frame (void* frame);

void* pmm_alloc_pages      (uint64_t count);
void* pmm_alloc_pages_flags(uint64_t count, uint64_t map_flags);
void  pmm_free_pages       (void* base, uint64_t count);

// Statistics / debug
unsigned long pmm_get_total_memory(void);
unsigned long pmm_get_free_memory (void);
unsigned long pmm_get_used_memory (void);
void          pmm_print_stats     (void);

uint64_t pmm_get_total_memory_kb(void);


// PMM ready flag (checked by heap & other subsystems)
int pmm_is_enabled(void);

#endif 