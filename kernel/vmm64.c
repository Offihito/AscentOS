// vmm64.c - Virtual Memory Manager Implementation
#include "vmm64.h"
#include "memory_unified.h"
#include <stdint.h>
#include <stddef.h>

// External functions
extern void* memset64(void* dest, int c, size_t n);
extern void* memcpy64(void* dest, const void* src, size_t n);
extern void serial_print(const char* str);

// Kernel address space
static address_space_t kernel_address_space;

// Statistics
static struct {
    uint64_t pages_mapped;
    uint64_t pages_unmapped;
    uint64_t page_faults;
    uint64_t tlb_flushes;
    uint64_t demand_allocations;
    uint64_t reserved_pages;
} vmm_stats = {0};

// Demand paging state
static int demand_paging_enabled = 0;

// Get current CR3 value
static inline uint64_t vmm_read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

// Set CR3 value
static inline void vmm_write_cr3(uint64_t cr3) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

// Invalidate TLB for single page
void vmm_flush_tlb_single(uint64_t virtual_addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_addr) : "memory");
    vmm_stats.tlb_flushes++;
}

// Flush entire TLB
void vmm_flush_tlb_all(void) {
    uint64_t cr3 = vmm_read_cr3();
    vmm_write_cr3(cr3);
    vmm_stats.tlb_flushes++;
}

// Allocate a page table
page_table_t* vmm_alloc_page_table(void) {
    void* frame = pmm_alloc_frame();
    if (!frame) {
        return NULL;
    }
    
    // Convert physical to virtual address for higher half kernel
    page_table_t* table = (page_table_t*)((uint64_t)frame + KERNEL_VMA - KERNEL_PHYS);
    memset64(table, 0, sizeof(page_table_t));
    return table;
}

// Free a page table
void vmm_free_page_table(page_table_t* table) {
    if (!table) return;
    
    // Convert virtual to physical
    uint64_t phys = (uint64_t)table - KERNEL_VMA + KERNEL_PHYS;
    pmm_free_frame((void*)phys);
}

// Get or create page table entry
static pte_t* vmm_get_pte(page_table_t* pml4, uint64_t virtual_addr, int create) {
    if (!pml4) return NULL;
    
    // PML4 entry
    uint64_t pml4_idx = PML4_INDEX(virtual_addr);
    pte_t* pml4e = &pml4->entries[pml4_idx];
    
    page_table_t* pdpt;
    if (!(*pml4e & PAGE_PRESENT)) {
        if (!create) return NULL;
        pdpt = vmm_alloc_page_table();
        if (!pdpt) return NULL;
        uint64_t pdpt_phys = (uint64_t)pdpt - KERNEL_VMA + KERNEL_PHYS;
        *pml4e = pdpt_phys | PAGE_PRESENT | PAGE_WRITE;
    } else {
        uint64_t pdpt_phys = PTE_GET_ADDR(*pml4e);
        pdpt = (page_table_t*)(pdpt_phys + KERNEL_VMA - KERNEL_PHYS);
    }
    
    // PDPT entry
    uint64_t pdpt_idx = PDPT_INDEX(virtual_addr);
    pte_t* pdpte = &pdpt->entries[pdpt_idx];
    
    page_table_t* pd;
    if (!(*pdpte & PAGE_PRESENT)) {
        if (!create) return NULL;
        pd = vmm_alloc_page_table();
        if (!pd) return NULL;
        uint64_t pd_phys = (uint64_t)pd - KERNEL_VMA + KERNEL_PHYS;
        *pdpte = pd_phys | PAGE_PRESENT | PAGE_WRITE;
    } else {
        // Check for 1GB page
        if (*pdpte & PAGE_SIZE_2M) {
            return NULL; // 1GB page, can't get 4KB PTE
        }
        uint64_t pd_phys = PTE_GET_ADDR(*pdpte);
        pd = (page_table_t*)(pd_phys + KERNEL_VMA - KERNEL_PHYS);
    }
    
    // PD entry
    uint64_t pd_idx = PD_INDEX(virtual_addr);
    pte_t* pde = &pd->entries[pd_idx];
    
    page_table_t* pt;
    if (!(*pde & PAGE_PRESENT)) {
        if (!create) return NULL;
        pt = vmm_alloc_page_table();
        if (!pt) return NULL;
        uint64_t pt_phys = (uint64_t)pt - KERNEL_VMA + KERNEL_PHYS;
        *pde = pt_phys | PAGE_PRESENT | PAGE_WRITE;
    } else {
        // Check for 2MB page
        if (*pde & PAGE_SIZE_2M) {
            return NULL; // 2MB page, can't get 4KB PTE
        }
        uint64_t pt_phys = PTE_GET_ADDR(*pde);
        pt = (page_table_t*)(pt_phys + KERNEL_VMA - KERNEL_PHYS);
    }
    
    // PT entry
    uint64_t pt_idx = PT_INDEX(virtual_addr);
    return &pt->entries[pt_idx];
}

// Initialize VMM
void vmm_init(void) {
    serial_print("VMM: Initializing virtual memory manager...\n");
    
    // Get current page table (set up by boot code)
    uint64_t cr3 = vmm_read_cr3();
    kernel_address_space.cr3_value = cr3;
    kernel_address_space.pml4 = (page_table_t*)(cr3 + KERNEL_VMA - KERNEL_PHYS);
    
    serial_print("VMM: Using existing page tables\n");
    serial_print("VMM: Initialization complete\n");
}

// Map a 4KB page
int vmm_map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    if (!VMM_IS_PAGE_ALIGNED(virtual_addr) || !VMM_IS_PAGE_ALIGNED(physical_addr)) {
        return -1; // Addresses must be page-aligned
    }
    
    pte_t* pte = vmm_get_pte(kernel_address_space.pml4, virtual_addr, 1);
    if (!pte) {
        return -1; // Failed to allocate page tables
    }
    
    // Set the page table entry
    *pte = (physical_addr & 0x000FFFFFFFFFF000ULL) | flags | PAGE_PRESENT;
    
    // Flush TLB for this page
    vmm_flush_tlb_single(virtual_addr);
    
    vmm_stats.pages_mapped++;
    return 0;
}

// Map a 2MB page
int vmm_map_page_2mb(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    if ((virtual_addr & (PAGE_SIZE_2MB - 1)) || (physical_addr & (PAGE_SIZE_2MB - 1))) {
        return -1; // Must be 2MB aligned
    }
    
    page_table_t* pml4 = kernel_address_space.pml4;
    
    // Get PML4 entry
    uint64_t pml4_idx = PML4_INDEX(virtual_addr);
    pte_t* pml4e = &pml4->entries[pml4_idx];
    
    page_table_t* pdpt;
    if (!(*pml4e & PAGE_PRESENT)) {
        pdpt = vmm_alloc_page_table();
        if (!pdpt) return -1;
        uint64_t pdpt_phys = (uint64_t)pdpt - KERNEL_VMA + KERNEL_PHYS;
        *pml4e = pdpt_phys | PAGE_PRESENT | PAGE_WRITE;
    } else {
        uint64_t pdpt_phys = PTE_GET_ADDR(*pml4e);
        pdpt = (page_table_t*)(pdpt_phys + KERNEL_VMA - KERNEL_PHYS);
    }
    
    // Get PDPT entry
    uint64_t pdpt_idx = PDPT_INDEX(virtual_addr);
    pte_t* pdpte = &pdpt->entries[pdpt_idx];
    
    page_table_t* pd;
    if (!(*pdpte & PAGE_PRESENT)) {
        pd = vmm_alloc_page_table();
        if (!pd) return -1;
        uint64_t pd_phys = (uint64_t)pd - KERNEL_VMA + KERNEL_PHYS;
        *pdpte = pd_phys | PAGE_PRESENT | PAGE_WRITE;
    } else {
        uint64_t pd_phys = PTE_GET_ADDR(*pdpte);
        pd = (page_table_t*)(pd_phys + KERNEL_VMA - KERNEL_PHYS);
    }
    
    // Set PD entry as 2MB page
    uint64_t pd_idx = PD_INDEX(virtual_addr);
    pd->entries[pd_idx] = (physical_addr & 0x000FFFFFFFE00000ULL) | 
                          flags | PAGE_SIZE_2M | PAGE_PRESENT;
    
    vmm_flush_tlb_single(virtual_addr);
    vmm_stats.pages_mapped++;
    
    return 0;
}

// Unmap a page
int vmm_unmap_page(uint64_t virtual_addr) {
    if (!VMM_IS_PAGE_ALIGNED(virtual_addr)) {
        return -1;
    }
    
    pte_t* pte = vmm_get_pte(kernel_address_space.pml4, virtual_addr, 0);
    if (!pte || !(*pte & PAGE_PRESENT)) {
        return -1; // Page not mapped
    }
    
    *pte = 0;
    vmm_flush_tlb_single(virtual_addr);
    vmm_stats.pages_unmapped++;
    
    return 0;
}

// Get physical address from virtual address
uint64_t vmm_get_physical_address(uint64_t virtual_addr) {
    pte_t* pte = vmm_get_pte(kernel_address_space.pml4, virtual_addr, 0);
    if (!pte || !(*pte & PAGE_PRESENT)) {
        return 0; // Page not mapped
    }
    
    uint64_t page_addr = PTE_GET_ADDR(*pte);
    uint64_t offset = virtual_addr & (PAGE_SIZE_4K - 1);
    return page_addr + offset;
}

// Map a range of pages
int vmm_map_range(uint64_t virtual_start, uint64_t physical_start, 
                  uint64_t size, uint64_t flags) {
    virtual_start = VMM_PAGE_ALIGN_DOWN(virtual_start);
    physical_start = VMM_PAGE_ALIGN_DOWN(physical_start);
    size = VMM_PAGE_ALIGN_UP(size);
    
    for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE_4K) {
        if (vmm_map_page(virtual_start + offset, physical_start + offset, flags) != 0) {
            return -1;
        }
    }
    
    return 0;
}

// Unmap a range of pages
int vmm_unmap_range(uint64_t virtual_start, uint64_t size) {
    virtual_start = VMM_PAGE_ALIGN_DOWN(virtual_start);
    size = VMM_PAGE_ALIGN_UP(size);
    
    for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE_4K) {
        vmm_unmap_page(virtual_start + offset);
    }
    
    return 0;
}

// Identity map a region
int vmm_identity_map(uint64_t physical_addr, uint64_t size, uint64_t flags) {
    return vmm_map_range(physical_addr, physical_addr, size, flags);
}

// Check if page is present
int vmm_is_page_present(uint64_t virtual_addr) {
    pte_t* pte = vmm_get_pte(kernel_address_space.pml4, virtual_addr, 0);
    return (pte && (*pte & PAGE_PRESENT)) ? 1 : 0;
}

// Get page flags
uint64_t vmm_get_page_flags(uint64_t virtual_addr) {
    pte_t* pte = vmm_get_pte(kernel_address_space.pml4, virtual_addr, 0);
    if (!pte) return 0;
    return *pte & 0xFFF;
}

// Set page flags
void vmm_set_page_flags(uint64_t virtual_addr, uint64_t flags) {
    pte_t* pte = vmm_get_pte(kernel_address_space.pml4, virtual_addr, 0);
    if (!pte) return;
    
    uint64_t addr = PTE_GET_ADDR(*pte);
    *pte = addr | (flags & 0xFFF) | PAGE_PRESENT;
    vmm_flush_tlb_single(virtual_addr);
}

// Create new address space
address_space_t* vmm_create_address_space(void) {
    address_space_t* space = (address_space_t*)kmalloc(sizeof(address_space_t));
    if (!space) return NULL;
    
    space->pml4 = vmm_alloc_page_table();
    if (!space->pml4) {
        kfree(space);
        return NULL;
    }
    
    // Copy kernel mappings (higher half)
    for (int i = 256; i < 512; i++) {
        space->pml4->entries[i] = kernel_address_space.pml4->entries[i];
    }
    
    uint64_t pml4_phys = (uint64_t)space->pml4 - KERNEL_VMA + KERNEL_PHYS;
    space->cr3_value = pml4_phys;
    
    return space;
}

// Destroy address space
void vmm_destroy_address_space(address_space_t* space) {
    if (!space || space == &kernel_address_space) return;
    
    // Free user-space page tables (indices 0-255)
    for (int i = 0; i < 256; i++) {
        if (space->pml4->entries[i] & PAGE_PRESENT) {
            // TODO: Recursively free page tables
        }
    }
    
    vmm_free_page_table(space->pml4);
    kfree(space);
}

// Switch address space
void vmm_switch_address_space(address_space_t* space) {
    if (!space) return;
    vmm_write_cr3(space->cr3_value);
}

// Get kernel address space
address_space_t* vmm_get_kernel_space(void) {
    return &kernel_address_space;
}

// Page fault handler
void vmm_page_fault_handler(uint64_t error_code, uint64_t faulting_addr) {
    vmm_stats.page_faults++;
    
    // Check if this is a demand paging scenario
    if (demand_paging_enabled) {
        // Get PTE for faulting address
        pte_t* pte = vmm_get_pte(kernel_address_space.pml4, faulting_addr, 0);
        
        // Check if page is reserved for demand allocation
        if (pte && (*pte & PAGE_RESERVED) && (*pte & PAGE_ON_DEMAND)) {
            // Extract flags from reserved entry
            uint64_t flags = *pte & 0xFFF;
            flags &= ~PAGE_RESERVED;
            flags &= ~PAGE_ON_DEMAND;
            
            // Allocate physical page
            void* phys_frame = pmm_alloc_frame();
            if (phys_frame) {
                // Map the page
                uint64_t page_aligned = VMM_PAGE_ALIGN_DOWN(faulting_addr);
                *pte = ((uint64_t)phys_frame & 0x000FFFFFFFFFF000ULL) | flags | PAGE_PRESENT;
                
                vmm_flush_tlb_single(page_aligned);
                vmm_stats.demand_allocations++;
                vmm_stats.pages_mapped++;
                
                serial_print("VMM: Demand paging - allocated page at 0x");
                // Page fault handled successfully
                return;
            } else {
                serial_print("VMM: Out of memory - cannot allocate demanded page\n");
            }
        }
    }
    
    serial_print("VMM: Page fault at 0x");
    // TODO: Print address in hex
    
    serial_print("\nError code: ");
    if (error_code & 0x1) serial_print("PRESENT ");
    if (error_code & 0x2) serial_print("WRITE ");
    if (error_code & 0x4) serial_print("USER ");
    if (error_code & 0x8) serial_print("RESERVED ");
    if (error_code & 0x10) serial_print("INSTRUCTION ");
    serial_print("\n");
    
    // Halt on unhandled page fault
    __asm__ volatile ("cli; hlt");
}

// Demand paging control functions
int vmm_enable_demand_paging(void) {
    demand_paging_enabled = 1;
    serial_print("VMM: Demand paging enabled\n");
    return 0;
}

int vmm_disable_demand_paging(void) {
    demand_paging_enabled = 0;
    serial_print("VMM: Demand paging disabled\n");
    return 0;
}

int vmm_is_demand_paging_enabled(void) {
    return demand_paging_enabled;
}

// Allocate page on demand
int vmm_allocate_on_demand(uint64_t virtual_addr, uint64_t flags) {
    if (!VMM_IS_PAGE_ALIGNED(virtual_addr)) {
        return -1;
    }
    
    pte_t* pte = vmm_get_pte(kernel_address_space.pml4, virtual_addr, 1);
    if (!pte) {
        return -1;
    }
    
    // Mark page as reserved for demand allocation
    // Store flags but don't set PRESENT bit
    *pte = (flags & 0xFFF) | PAGE_RESERVED | PAGE_ON_DEMAND;
    
    vmm_stats.reserved_pages++;
    return 0;
}

// Reserve pages without allocating physical memory
int vmm_reserve_pages(uint64_t virtual_start, uint64_t count, uint64_t flags) {
    if (!VMM_IS_PAGE_ALIGNED(virtual_start)) {
        return -1;
    }
    
    for (uint64_t i = 0; i < count; i++) {
        uint64_t vaddr = virtual_start + (i * PAGE_SIZE_4K);
        if (vmm_allocate_on_demand(vaddr, flags) != 0) {
            return -1;
        }
    }
    
    return 0;
}

// Commit a reserved page (allocate physical memory now)
int vmm_commit_page(uint64_t virtual_addr) {
    if (!VMM_IS_PAGE_ALIGNED(virtual_addr)) {
        return -1;
    }
    
    pte_t* pte = vmm_get_pte(kernel_address_space.pml4, virtual_addr, 0);
    if (!pte) {
        return -1;
    }
    
    // Check if page is reserved
    if (!(*pte & PAGE_RESERVED)) {
        return -1; // Not a reserved page
    }
    
    // Extract flags
    uint64_t flags = *pte & 0xFFF;
    flags &= ~PAGE_RESERVED;
    flags &= ~PAGE_ON_DEMAND;
    
    // Allocate physical frame
    void* phys_frame = pmm_alloc_frame();
    if (!phys_frame) {
        return -1; // Out of memory
    }
    
    // Map the page
    *pte = ((uint64_t)phys_frame & 0x000FFFFFFFFFF000ULL) | flags | PAGE_PRESENT;
    
    vmm_flush_tlb_single(virtual_addr);
    vmm_stats.pages_mapped++;
    vmm_stats.reserved_pages--;
    
    return 0;
}

// Commit a range of reserved pages
int vmm_commit_range(uint64_t virtual_start, uint64_t count) {
    if (!VMM_IS_PAGE_ALIGNED(virtual_start)) {
        return -1;
    }
    
    for (uint64_t i = 0; i < count; i++) {
        uint64_t vaddr = virtual_start + (i * PAGE_SIZE_4K);
        if (vmm_commit_page(vaddr) != 0) {
            return -1;
        }
    }
    
    return 0;
}

// Print statistics
void vmm_print_stats(void) {
    serial_print("VMM Statistics:\n");
    serial_print("  Pages mapped: ");
    // TODO: Print number
    serial_print("\n  Pages unmapped: ");
    // TODO: Print number
    serial_print("\n  Page faults: ");
    // TODO: Print number
    serial_print("\n  TLB flushes: ");
    // TODO: Print number
    serial_print("\n");
}

// Get statistics
uint64_t vmm_get_pages_mapped(void) {
    return vmm_stats.pages_mapped;
}

uint64_t vmm_get_pages_unmapped(void) {
    return vmm_stats.pages_unmapped;
}

uint64_t vmm_get_page_faults(void) {
    return vmm_stats.page_faults;
}

uint64_t vmm_get_tlb_flushes(void) {
    return vmm_stats.tlb_flushes;
}

uint64_t vmm_get_demand_allocations(void) {
    return vmm_stats.demand_allocations;
}

uint64_t vmm_get_reserved_pages(void) {
    return vmm_stats.reserved_pages;
}

// Dump page tables for debugging
void vmm_dump_page_tables(uint64_t virtual_addr) {
    serial_print("Page table dump for virtual address: 0x");
    // TODO: Print address
    serial_print("\n");
    
    page_table_t* pml4 = kernel_address_space.pml4;
    
    uint64_t pml4_idx = PML4_INDEX(virtual_addr);
    serial_print("  PML4 index: ");
    // TODO: Print index
    
    pte_t pml4e = pml4->entries[pml4_idx];
    if (!(pml4e & PAGE_PRESENT)) {
        serial_print("    Not present\n");
        return;
    }
    
    serial_print("    Present, addr: 0x");
    // TODO: Print address
    serial_print("\n");
}