# Memory Management Subsystem Rewrite Plan

## Goal
Rewrite PMM, VMM, heap allocator, and VMA manager from scratch to robustly support TCC compiling programs with malloc/free without issues.

## Current Issues Analysis

### PMM (Physical Memory Manager) - [kernel/src/mm/pmm.c]
**Current Implementation:** Simple bitmap allocator with linear search
**Problems:**
- O(n) allocation time - scans bitmap linearly from `last_scanned_page`
- No buddy system or frame tracking for efficient multi-page allocations
- Fragmentation over time as allocations/free patterns vary
- `pmm_free()` doesn't accept count parameter in some code paths
- Bitmap itself consumes significant memory for large RAM

### VMM (Virtual Memory Manager) - [kernel/src/mm/vmm.c]
**Current Implementation:** Basic 4-level page table walker with HHDM access
**Problems:**
- Single global `vmm_lock` creates contention
- No page fault handler for demand paging
- `vmm_unmap_page()` doesn't free empty intermediate page tables
- No support for huge pages (2MB/1GB)
- Deep clone operations don't handle OOM rollback properly

### Heap Allocator - [kernel/src/mm/heap.c]
**Current Implementation:** Simple linked-list first-fit with splitting/coalescing
**Problems:**
- O(n) search through entire linked list for every allocation
- No size classes or segregated lists - causes fragmentation
- Only supports kernel space (uses PMM directly, not VMM mappings)
- No user-space heap support (TCC's musl libc needs brk/mmap)
- Missing boundary tags for better coalescing

### VMA (Virtual Memory Area) - [kernel/src/mm/vma.c]
**Current Implementation:** Fixed-size array (1024 slots) with linear search
**Problems:**
- Fixed limit of 1024 regions may be insufficient for complex programs
- O(n) search for find/overlap operations
- No red-black tree or interval tree for efficient operations
- VMA splitting logic has edge cases that may fail
- No merge of adjacent VMAs with same properties

## Rewrite Strategy

### Phase 1: PMM - Buddy Allocator with Bitmap Backend
**Files:** `kernel/src/mm/pmm.c`, `kernel/src/mm/pmm.h`

**Design:**
- Hybrid approach: Bitmap for tracking + Buddy system for allocations
- Support orders 0-10 (4KB to 4MB allocations)
- Maintain free lists for each order
- Split larger blocks when smaller ones requested
- Coalesce buddies on free

**Implementation Steps:**
1. Define buddy allocator data structures:
   ```c
   #define MAX_ORDER 11  // 4KB * 2^10 = 4MB max
   struct buddy_zone {
       uint64_t start_pfn;
       uint64_t end_pfn;
       struct list_head free_list[MAX_ORDER];
       spinlock_t lock;
   };
   ```

2. Initialize buddy zones from memory map
3. Implement `buddy_alloc(order)` with splitting logic
4. Implement `buddy_free(pfn, order)` with coalescing
5. Maintain bitmap as fallback/verification
6. Add statistics tracking (free pages, fragmentation ratio)

**API Changes:**
```c
void *pmm_alloc_page(void);                    // Order 0
void *pmm_alloc_pages(size_t count);           // Multi-page
void pmm_free_page(void *ptr);                 // Free single
void pmm_free_pages(void *ptr, size_t count);  // Free multiple
size_t pmm_get_free_pages(void);               // Statistics
```

### Phase 2: VMM - Per-Process Page Tables with Demand Paging
**Files:** `kernel/src/mm/vmm.c`, `kernel/src/mm/vmm.h`, `kernel/src/interrupt/isr.c`

**Design:**
- Refine page table management with proper locking
- Add page fault handler for demand paging
- Support for huge pages (2MB) for kernel mappings
- Proper cleanup of empty page table levels
- Virtual address space layout definition

**Implementation Steps:**
1. Define virtual memory layout:
   ```c
   #define USER_SPACE_BASE    0x0000000000000000
   #define USER_SPACE_LIMIT   0x00007FFFFFFFFFFF
   #define KERNEL_SPACE_BASE  0xFFFF800000000000
   #define HHDM_BASE          0xFFFF800000000000
   #define VMAP_BASE          0xFFFFC00000000000
   #define KERNEL_HEAP_BASE   0xFFFFE00000000000
   ```

2. Implement page fault handler:
   - Decode CR2 (faulting address) and error code
   - Check if address belongs to valid VMA
   - If VMA exists but page not present: allocate and map
   - If invalid address: send SIGSEGV to process

3. Improve `vmm_map_page()`:
   - Add support for huge pages
   - Better error handling
   - Check for existing mappings

4. Implement `vmm_unmap_page()`:
   - Free empty intermediate tables
   - Handle huge page demotion

5. Add `vmm_map_range()` for bulk mappings

**New APIs:**
```c
int vmm_handle_page_fault(uint64_t cr2, uint64_t error_code);
bool vmm_map_huge_page(uint64_t *pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags);
void vmm_free_empty_tables(uint64_t *pml4, uint64_t vaddr);
int vmm_map_range(uint64_t *pml4, uint64_t vaddr, uint64_t paddr, size_t pages, uint64_t flags);
```

### Phase 3: Kernel Heap - Slab Allocator with Multiple Size Classes
**Files:** `kernel/src/mm/heap.c`, `kernel/src/mm/heap.h`

**Design:**
- Implement slab allocator for common sizes
- Maintain multiple free lists (segregated storage)
- Use VMM for virtual address space management
- Support both kernel and future user-space heaps

**Implementation Steps:**
1. Define size classes (powers of 2, aligned to 16 bytes):
   ```c
   #define SIZE_CLASSES 16
   // 16, 32, 64, 128, 256, 512, 1K, 2K, 4K, 8K, 16K, 32K, 64K, 128K, 256K, 512K
   ```

2. Implement slab structure:
   ```c
   struct slab {
       void *base;              // Base address
       size_t size_class;       // Which size class
       size_t used;             // Objects in use
       struct slab *next;       // Next slab in list
       struct slab *prev;
       uint8_t bitmap[];        // Allocation bitmap
   };
   ```

3. Implement allocation logic:
   - Find appropriate size class
   - Search partial slabs first
   - Allocate new slab from VMM if needed
   - Return object from slab

4. Implement free logic:
   - Find slab from pointer
   - Mark object free in bitmap
   - Free slab if completely empty
   - Coalesce with adjacent free regions if possible

5. Add kernel heap virtual region:
   - Use `vmm_map_range()` to create heap area
   - Grow heap dynamically with VMM
   - Maintain heap metadata in dedicated pages

**API Changes:**
```c
void *kmalloc(size_t size);
void kfree(void *ptr);
void *krealloc(void *ptr, size_t new_size);
void *kcalloc(size_t num, size_t size);
void heap_print_stats(void);  // Debug
```

### Phase 4: VMA - Interval Tree with Merge/Split Operations
**Files:** `kernel/src/mm/vma.c`, `kernel/src/mm/vma.h`

**Design:**
- Replace fixed array with red-black tree or interval tree
- Efficient O(log n) operations
- Automatic merging of adjacent compatible VMAs
- Proper splitting on munmap/mprotect

**Implementation Steps:**
1. Implement red-black tree (or use interval tree):
   ```c
   struct vma {
       uint64_t start;
       uint64_t end;
       uint64_t prot;
       uint64_t flags;
       int fd;
       uint64_t offset;
       
       // Tree nodes
       struct vma *rb_parent;
       struct vma *rb_left;
       struct vma *rb_right;
       int rb_color;  // RED or BLACK
   };
   ```

2. Implement tree operations:
   - `vma_insert()` - Insert with overlap checking
   - `vma_remove()` - Remove with splitting
   - `vma_find()` - Find VMA containing address
   - `vma_find_overlap()` - Check for overlaps
   - `vma_merge()` - Merge adjacent compatible VMAs

3. Implement VMA splitting:
   - Handle partial unmapping
   - Create new VMA for remaining region
   - Update page tables accordingly

4. Add VMA iterator for debugging:
   ```c
   void vma_dump_list(struct vma_list *list);
   ```

**API Changes:**
```c
struct vma *vma_insert(struct vma_list *list, uint64_t start, uint64_t end, 
                       uint64_t prot, uint64_t flags, int fd, uint64_t offset);
bool vma_remove(struct vma_list *list, uint64_t start, uint64_t end);
struct vma *vma_find(struct vma_list *list, uint64_t addr);
void vma_merge_adjacent(struct vma_list *list);
```

### Phase 5: Integration & User-Space Heap Support
**Files:** `kernel/src/syscalls/sys_mm.c`, `kernel/src/sched/process.c`

**Focus:** Ensure TCC compilation works correctly

**Implementation Steps:**
1. Improve `sys_brk()`:
   - Proper VMA tracking for heap region
   - Handle shrinking correctly (unmap pages)
   - Validate against other VMAs

2. Improve `sys_mmap()`:
   - Better address selection (find gap in VMA tree)
   - Support MAP_FIXED correctly
   - Handle file-backed mappings
   - Proper error codes

3. Improve `sys_munmap()`:
   - Split VMAs correctly
   - Unmap pages from page tables
   - Free physical frames for private anonymous mappings
   - Merge remaining VMAs if possible

4. Add `sys_mprotect()`:
   - Change protection flags
   - Split VMAs if needed
   - Update page table entries

5. Test with TCC compilation:
   - Monitor brk/mmap calls during compilation
   - Verify malloc/free patterns work
   - Check for memory leaks
   - Test with complex programs

### Phase 6: Testing & Validation
**Files:** Test programs in `userland/`, shell commands

**Test Suite:**
1. **PMM Tests:**
   - Allocate/free patterns (sequential, random)
   - Stress test with fragmentation
   - Multi-page allocations
   - Boundary conditions

2. **VMM Tests:**
   - Map/unmap cycles
   - Page fault handling
   - Fork with copy-on-write
   - Huge page mappings

3. **Heap Tests:**
   - Allocate various sizes
   - Fragmentation test (alloc/free patterns)
   - Stress test (thousands of allocs)
   - Memory corruption detection

4. **VMA Tests:**
   - Overlapping mappings
   - Split and merge operations
   - munmap partial regions
   - mprotect changes

5. **TCC Integration Tests:**
   - Compile hello world
   - Compile program with malloc/free
   - Compile larger programs (kilo, raycast)
   - Run compiled programs
   - Check for memory corruption

6. **Stress Tests:**
   - Run multiple TCC compilations simultaneously
   - Allocate/free in tight loops
   - Fork bomb (limited) to test process memory
   - Memory exhaustion handling

## File Modification Order

1. `kernel/src/mm/pmm.h` - New API definitions
2. `kernel/src/mm/pmm.c` - Buddy allocator implementation
3. `kernel/src/mm/vmm.h` - New VMM APIs
4. `kernel/src/mm/vmm.c` - Page fault handler, improved mapping
5. `kernel/src/interrupt/isr.c` - Register page fault handler
6. `kernel/src/mm/heap.h` - Slab allocator API
7. `kernel/src/mm/heap.c` - Slab allocator implementation
8. `kernel/src/mm/vma.h` - Tree-based VMA API
9. `kernel/src/mm/vma.c` - Red-black tree VMA implementation
10. `kernel/src/syscalls/sys_mm.c` - Improved syscalls
11. `kernel/src/sched/process.c` - Process memory management
12. `kernel/src/kernel.c` - Update initialization order

## Key Design Decisions

1. **Buddy vs Bitmap for PMM:** Buddy system provides better performance for multi-page allocations and reduces fragmentation, while bitmap is simpler but slower.

2. **Slab vs Linked List for Heap:** Slab allocator provides O(1) allocation for common sizes and better cache locality, at the cost of complexity.

3. **Red-Black Tree for VMA:** Provides O(log n) operations vs O(n) for array, essential for complex programs with many mappings.

4. **Demand Paging:** Implementing page fault handler allows lazy allocation, reducing memory pressure and improving performance.

## Success Criteria

- TCC can compile programs using malloc/free without crashes
- No memory leaks in kernel after extended use
- Page fault handler correctly handles all cases
- VMA operations are correct (merge, split, overlap detection)
- Performance improvement in allocation operations
- Stress tests pass without corruption or OOM errors
