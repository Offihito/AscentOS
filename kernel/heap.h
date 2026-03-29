#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>
#include "spinlock64.h"

// Heap layout constants (shared with vmm / boot code if needed)
#define PAGE_SIZE          4096
#define INITIAL_HEAP_SIZE  (4  * 1024 * 1024)   
#define MAX_HEAP_SIZE      (64 * 1024 * 1024)   
#define HEAP_START         0x200000             
#define HEAP_EXPAND_SIZE   (1  * 1024 * 1024)   

void init_heap(void);      
void init_memory_unified(void); 
void init_memory64(void);      
void init_memory_gui(void);     


// Core allocator API
void* kmalloc (size_t size);
void  kfree   (void*  ptr);
void* krealloc(void*  ptr,  size_t new_size);
void* kcalloc (size_t num,  size_t size);

void* malloc_gui(uint64_t size);
void  free_gui  (void*    ptr);

// sbrk / brk interface (SYS_SBRK syscall support)
uint64_t kmalloc_get_brk(void);
uint64_t kmalloc_set_brk(uint64_t new_brk);

void show_memory_info  (void);
void set_static_heap_mode(int enable);

// ============================================================================
// Slab Allocator
// ============================================================================

// Fixed object sizes served by slab caches (bytes)
#define SLAB_SIZE_8      8
#define SLAB_SIZE_16     16
#define SLAB_SIZE_32     32
#define SLAB_SIZE_64     64
#define SLAB_SIZE_128    128
#define SLAB_SIZE_256    256
#define SLAB_SIZE_512    512
#define SLAB_SIZE_1024   1024
#define SLAB_NUM_CACHES  8        // number of size classes

#define SLAB_OBJECTS_PER_SLAB  32   // objects per backing slab
#define SLAB_MAGIC             0x51AB51AB

typedef struct slab_buf {
    uint32_t          magic;
    uint32_t          obj_size;       // usable object size
    uint32_t          capacity;       // max objects
    uint32_t          used;           // allocated objects
    uint8_t*          bitmap;         // 1 bit per slot (1 = used)
    uint8_t*          data;           // object storage
    struct slab_buf*  next;           // next slab in cache list
} slab_t;

typedef struct {
    uint32_t  obj_size;
    slab_t*   head;                   // linked list of slabs
    spinlock_t lock;                  // per-cache IRQ-safe spinlock
    // stats
    uint64_t  total_allocs;
    uint64_t  total_frees;
    uint64_t  active_objects;
    uint64_t  slab_count;
} slab_cache_t;

void  slab_init        (void);
void* slab_alloc       (uint32_t size);
void  slab_free        (void* ptr);
void  slab_stats       (void);            // pretty-print to VGA/serial
void  slab_stats_output(void* output);    // write stats into CommandOutput*
int   slab_owns        (void* ptr);       // 1 if ptr is inside any slab
// Query a single cache by index (for display in command handlers)
void  slab_query_cache (int index,
                        uint32_t* out_obj_size,
                        uint64_t* out_slabs,
                        uint64_t* out_active,
                        uint64_t* out_allocs,
                        uint64_t* out_frees);

// Exported pointers (legacy consumers)
extern uint8_t* heap_start;
extern uint8_t* heap_current;

// Misc helpers
void*    map_page       (uint64_t physical, uint64_t virtual_addr);
uint64_t get_total_memory(void);

#endif