// vmm64.h - Virtual Memory Manager for Higher Half Kernel
#ifndef VMM64_H
#define VMM64_H

#include <stdint.h>
#include <stddef.h>

// Higher half kernel base
#define KERNEL_VMA      0xFFFFFFFF80000000ULL
#define KERNEL_PHYS     0x100000ULL

// Page table entry flags
#define PAGE_PRESENT    (1ULL << 0)
#define PAGE_WRITE      (1ULL << 1)
#define PAGE_USER       (1ULL << 2)
#define PAGE_ACCESSED   (1ULL << 5)
#define PAGE_DIRTY      (1ULL << 6)
#define PAGE_SIZE_2M    (1ULL << 7)
#define PAGE_GLOBAL     (1ULL << 8)
#define PAGE_NX         (1ULL << 63)

// Custom flags for demand paging (use available bits 9-11)
#define PAGE_RESERVED   (1ULL << 9)   // Page is reserved but not allocated
#define PAGE_ON_DEMAND  (1ULL << 10)  // Allocate on first access

// Page sizes
#define PAGE_SIZE_4K    4096
#define PAGE_SIZE_2MB   (2 * 1024 * 1024)
#define PAGE_SIZE_1GB   (1024 * 1024 * 1024)

// Page table levels
#define PML4_INDEX(addr) (((addr) >> 39) & 0x1FF)
#define PDPT_INDEX(addr) (((addr) >> 30) & 0x1FF)
#define PD_INDEX(addr)   (((addr) >> 21) & 0x1FF)
#define PT_INDEX(addr)   (((addr) >> 12) & 0x1FF)

// Page table entry
typedef uint64_t pte_t;

// Page directory structures
typedef struct {
    pte_t entries[512];
} __attribute__((aligned(4096))) page_table_t;

// Address space structure
typedef struct {
    page_table_t* pml4;
    uint64_t cr3_value;
} address_space_t;

// VMM initialization
void vmm_init(void);

// Page mapping functions
int vmm_map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);
int vmm_unmap_page(uint64_t virtual_addr);
uint64_t vmm_get_physical_address(uint64_t virtual_addr);

// Large page mapping
int vmm_map_page_2mb(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);

// Address space management
address_space_t* vmm_create_address_space(void);
void vmm_destroy_address_space(address_space_t* space);
void vmm_switch_address_space(address_space_t* space);
address_space_t* vmm_get_kernel_space(void);

// Memory region mapping
int vmm_map_range(uint64_t virtual_start, uint64_t physical_start, 
                  uint64_t size, uint64_t flags);
int vmm_unmap_range(uint64_t virtual_start, uint64_t size);

// Identity mapping helpers
int vmm_identity_map(uint64_t physical_addr, uint64_t size, uint64_t flags);

// Page fault handler
void vmm_page_fault_handler(uint64_t error_code, uint64_t faulting_addr);

// Demand paging functions
int vmm_enable_demand_paging(void);
int vmm_disable_demand_paging(void);
int vmm_is_demand_paging_enabled(void);

// Allocate page on demand
int vmm_allocate_on_demand(uint64_t virtual_addr, uint64_t flags);

// Reserve virtual memory without backing physical pages
int vmm_reserve_pages(uint64_t virtual_start, uint64_t count, uint64_t flags);

// Commit reserved pages (allocate physical memory)
int vmm_commit_page(uint64_t virtual_addr);
int vmm_commit_range(uint64_t virtual_start, uint64_t count);

// TLB management
void vmm_flush_tlb_single(uint64_t virtual_addr);
void vmm_flush_tlb_all(void);

// Utility functions
int vmm_is_page_present(uint64_t virtual_addr);
uint64_t vmm_get_page_flags(uint64_t virtual_addr);
void vmm_set_page_flags(uint64_t virtual_addr, uint64_t flags);

// Statistics and debugging
void vmm_print_stats(void);
void vmm_dump_page_tables(uint64_t virtual_addr);

// Statistics getters
uint64_t vmm_get_pages_mapped(void);
uint64_t vmm_get_pages_unmapped(void);
uint64_t vmm_get_page_faults(void);
uint64_t vmm_get_tlb_flushes(void);
uint64_t vmm_get_demand_allocations(void);
uint64_t vmm_get_reserved_pages(void);

// Page table allocation
page_table_t* vmm_alloc_page_table(void);
void vmm_free_page_table(page_table_t* table);

// Helper macros
#define VMM_PAGE_ALIGN_DOWN(addr) ((addr) & ~(PAGE_SIZE_4K - 1))
#define VMM_PAGE_ALIGN_UP(addr)   (((addr) + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1))
#define VMM_IS_PAGE_ALIGNED(addr) (((addr) & (PAGE_SIZE_4K - 1)) == 0)

// Extract physical address from PTE
#define PTE_GET_ADDR(pte) ((pte) & 0x000FFFFFFFFFF000ULL)

#endif // VMM64_H