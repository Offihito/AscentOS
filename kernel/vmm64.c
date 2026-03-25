#include "vmm64.h"
#include "pmm.h"
#include "heap.h"
#include "cpu64.h"
#include <stdint.h>
#include <stddef.h>

extern void* memset64(void* dest, int c, size_t n);
extern void* memcpy64(void* dest, const void* src, size_t n);
extern void serial_print(const char* str);

/* ── Phys ↔ Virt yardımcıları ────────────────────────────────────────────── */
static inline uint64_t phys_to_virt(uint64_t phys) { return phys + KERNEL_VMA; }
static inline uint64_t virt_to_phys(uint64_t virt) { return virt - KERNEL_VMA; }

/* ── Kernel adres alanı ──────────────────────────────────────────────────── */
static address_space_t kernel_address_space;

/* ── İstatistikler ───────────────────────────────────────────────────────── */
static struct {
    uint64_t pages_mapped;
    uint64_t pages_unmapped;
    uint64_t page_faults;
    uint64_t tlb_flushes;
    uint64_t demand_allocations;
    uint64_t reserved_pages;
} vmm_stats = {0};

static int demand_paging_enabled = 0;

/* ── CR3 erişimi ─────────────────────────────────────────────────────────── */
static inline uint64_t vmm_read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}
static inline void vmm_write_cr3(uint64_t cr3) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* ── TLB ─────────────────────────────────────────────────────────────────── */
void vmm_flush_tlb_single(uint64_t virtual_addr) {
    cpu_invlpg(virtual_addr);
    vmm_stats.tlb_flushes++;
}
void vmm_flush_tlb_all(void) {
    uint64_t cr3 = vmm_read_cr3();
    vmm_write_cr3(cr3);
    vmm_stats.tlb_flushes++;
}

/* ── Sayfa tablosu tahsisi ───────────────────────────────────────────────── */
page_table_t* vmm_alloc_page_table(void) {
    void* frame = pmm_alloc_frame();
    if (!frame) return NULL;
    /* Fiziksel → sanal: phys + KERNEL_VMA */
    page_table_t* table = (page_table_t*)phys_to_virt((uint64_t)frame);
    memset64(table, 0, sizeof(page_table_t));
    return table;
}

void vmm_free_page_table(page_table_t* table) {
    if (!table) return;
    /* Sanal → fiziksel: virt - KERNEL_VMA */
    pmm_free_frame((void*)virt_to_phys((uint64_t)table));
}

/* ── PTE al / oluştur ────────────────────────────────────────────────────── */
static pte_t* vmm_get_pte(page_table_t* pml4, uint64_t virtual_addr, int create) {
    if (!pml4) return NULL;

    /* PML4 */
    uint64_t pml4_idx = PML4_INDEX(virtual_addr);
    pte_t* pml4e = &pml4->entries[pml4_idx];

    page_table_t* pdpt;
    if (!(*pml4e & PAGE_PRESENT)) {
        if (!create) return NULL;
        pdpt = vmm_alloc_page_table();
        if (!pdpt) return NULL;
        uint64_t pdpt_phys = virt_to_phys((uint64_t)pdpt);
        *pml4e = pdpt_phys | PAGE_PRESENT | PAGE_WRITE |
                 (pml4_idx < 256 ? PAGE_USER : 0);
    } else {
        uint64_t pdpt_phys = PTE_GET_ADDR(*pml4e);
        uint64_t pdpt_virt = phys_to_virt(pdpt_phys);
        /* Kanonik adres kontrolü */
        uint64_t top = pdpt_virt >> 48;
        if (top != 0x0000 && top != 0xFFFF) {
            if (!create) return NULL;
            pdpt = vmm_alloc_page_table();
            if (!pdpt) return NULL;
            uint64_t new_phys = virt_to_phys((uint64_t)pdpt);
            *pml4e = new_phys | PAGE_PRESENT | PAGE_WRITE |
                     (pml4_idx < 256 ? PAGE_USER : 0);
        } else {
            pdpt = (page_table_t*)pdpt_virt;
        }
    }

    /* PDPT */
    uint64_t pdpt_idx = PDPT_INDEX(virtual_addr);
    pte_t* pdpte = &pdpt->entries[pdpt_idx];

    page_table_t* pd;
    if (!(*pdpte & PAGE_PRESENT)) {
        if (!create) return NULL;
        pd = vmm_alloc_page_table();
        if (!pd) return NULL;
        uint64_t pd_phys = virt_to_phys((uint64_t)pd);
        *pdpte = pd_phys | PAGE_PRESENT | PAGE_WRITE;
    } else {
        if (*pdpte & PAGE_SIZE_2M) return NULL; /* 1GB page */
        uint64_t pd_phys = PTE_GET_ADDR(*pdpte);
        pd = (page_table_t*)phys_to_virt(pd_phys);
    }

    /* PD */
    uint64_t pd_idx = PD_INDEX(virtual_addr);
    pte_t* pde = &pd->entries[pd_idx];

    page_table_t* pt;
    if (!(*pde & PAGE_PRESENT)) {
        if (!create) return NULL;
        pt = vmm_alloc_page_table();
        if (!pt) return NULL;
        uint64_t pt_phys = virt_to_phys((uint64_t)pt);
        *pde = pt_phys | PAGE_PRESENT | PAGE_WRITE;
    } else {
        if (*pde & PAGE_SIZE_2M) return NULL; /* 2MB page */
        uint64_t pt_phys = PTE_GET_ADDR(*pde);
        pt = (page_table_t*)phys_to_virt(pt_phys);
    }

    return &pt->entries[PT_INDEX(virtual_addr)];
}

/* ── vmm_init ────────────────────────────────────────────────────────────── */
void vmm_init(void) {
    serial_print("VMM: Initializing virtual memory manager...\n");

    /* Boot page table'ı mevcut CR3'ten al.
     * CR3 = fiziksel adres → sanal = CR3 + KERNEL_VMA */
    uint64_t cr3 = vmm_read_cr3();
    kernel_address_space.cr3_value = cr3;
    kernel_address_space.pml4 = (page_table_t*)phys_to_virt(cr3);

    serial_print("VMM: Using existing boot page tables\n");

    /* ── Lower-half (PML4[0..255]) KORUNUYOR ─────────────────────────────── *
     *                                                                         *
     * Boot kodu PML4[0] üzerinden tüm fiziksel belleği (0..4 GB) kimlik      *
     * eşlemesiyle erişilebilir kılar.  Bu eşlemeyi silmek şunları kırar:     *
     *                                                                         *
     *  · Heap     (0x200000)  kmalloc/kfree     → #PF                        *
     *  · VGA      (0xB8000)   vesa64 text buf   → #PF                        *
     *  · Framebuf (0xFD000000) phys_to_virt 4GB+ overflow eder → #PF        *
     *  · PMM bitmap (kernel+) pmm_alloc_frame   → #PF                        *
     *                                                                         *
     * phys_to_virt(x) = x + KERNEL_VMA; x >= ~2 GB için sonuç kanonik       *
     * adres aralığını aşar (64-bit overflow). Framebuffer ~4 GB fiziksel     *
     * adresinde olduğundan higher-half üzerinden erişilemez; kimlik           *
     * eşlemesi zorunludur.                                                    *
     *                                                                         *
     * Güvenlik notu: user-space izolasyonu PML4[0] silmekle değil,           *
     * vmm_create_address_space() ile her task için ayrı PML4 üretilerek       *
     * sağlanır — kernel PML4'ü hiçbir zaman user CR3'e konulmaz.             */

    serial_print("VMM: Initialization complete\n");
}

/* ── 4KB sayfa eşleme ────────────────────────────────────────────────────── */
int vmm_map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    if (!VMM_IS_PAGE_ALIGNED(virtual_addr) || !VMM_IS_PAGE_ALIGNED(physical_addr))
        return -1;

    pte_t* pte = vmm_get_pte(kernel_address_space.pml4, virtual_addr, 1);
    if (!pte) return -1;

    *pte = (physical_addr & 0x000FFFFFFFFFF000ULL) | flags | PAGE_PRESENT;
    vmm_flush_tlb_single(virtual_addr);
    vmm_stats.pages_mapped++;
    return 0;
}

/* ── 2MB sayfa eşleme ────────────────────────────────────────────────────── */
int vmm_map_page_2mb(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    if ((virtual_addr & (PAGE_SIZE_2MB - 1)) || (physical_addr & (PAGE_SIZE_2MB - 1)))
        return -1;

    page_table_t* pml4 = kernel_address_space.pml4;

    uint64_t pml4_idx = PML4_INDEX(virtual_addr);
    pte_t* pml4e = &pml4->entries[pml4_idx];

    page_table_t* pdpt;
    if (!(*pml4e & PAGE_PRESENT)) {
        pdpt = vmm_alloc_page_table();
        if (!pdpt) return -1;
        uint64_t pdpt_phys = virt_to_phys((uint64_t)pdpt);
        *pml4e = pdpt_phys | PAGE_PRESENT | PAGE_WRITE;
    } else {
        pdpt = (page_table_t*)phys_to_virt(PTE_GET_ADDR(*pml4e));
    }

    uint64_t pdpt_idx = PDPT_INDEX(virtual_addr);
    pte_t* pdpte = &pdpt->entries[pdpt_idx];

    page_table_t* pd;
    if (!(*pdpte & PAGE_PRESENT)) {
        pd = vmm_alloc_page_table();
        if (!pd) return -1;
        uint64_t pd_phys = virt_to_phys((uint64_t)pd);
        *pdpte = pd_phys | PAGE_PRESENT | PAGE_WRITE;
    } else {
        pd = (page_table_t*)phys_to_virt(PTE_GET_ADDR(*pdpte));
    }

    uint64_t pd_idx = PD_INDEX(virtual_addr);
    pd->entries[pd_idx] = (physical_addr & 0x000FFFFFFFE00000ULL) |
                          flags | PAGE_SIZE_2M | PAGE_PRESENT;

    vmm_flush_tlb_single(virtual_addr);
    vmm_stats.pages_mapped++;
    return 0;
}

/* ── Sayfa silme ─────────────────────────────────────────────────────────── */
int vmm_unmap_page(uint64_t virtual_addr) {
    if (!VMM_IS_PAGE_ALIGNED(virtual_addr)) return -1;

    pte_t* pte = vmm_get_pte(kernel_address_space.pml4, virtual_addr, 0);
    if (!pte || !(*pte & PAGE_PRESENT)) return -1;

    *pte = 0;
    vmm_flush_tlb_single(virtual_addr);
    vmm_stats.pages_unmapped++;
    return 0;
}

/* ── Sanal → fiziksel ────────────────────────────────────────────────────── */
uint64_t vmm_get_physical_address(uint64_t virtual_addr) {
    pte_t* pte = vmm_get_pte(kernel_address_space.pml4, virtual_addr, 0);
    if (!pte || !(*pte & PAGE_PRESENT)) return 0;

    uint64_t page_addr = PTE_GET_ADDR(*pte);
    uint64_t offset    = virtual_addr & (PAGE_SIZE_4K - 1);
    return page_addr + offset;
}

/* ── Aralık eşleme ───────────────────────────────────────────────────────── */
int vmm_map_range(uint64_t virtual_start, uint64_t physical_start,
                  uint64_t size, uint64_t flags) {
    virtual_start  = VMM_PAGE_ALIGN_DOWN(virtual_start);
    physical_start = VMM_PAGE_ALIGN_DOWN(physical_start);
    size           = VMM_PAGE_ALIGN_UP(size);

    for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE_4K) {
        if (vmm_map_page(virtual_start + offset,
                         physical_start + offset, flags) != 0)
            return -1;
    }
    return 0;
}

int vmm_unmap_range(uint64_t virtual_start, uint64_t size) {
    virtual_start = VMM_PAGE_ALIGN_DOWN(virtual_start);
    size          = VMM_PAGE_ALIGN_UP(size);
    for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE_4K)
        vmm_unmap_page(virtual_start + offset);
    return 0;
}

int vmm_identity_map(uint64_t physical_addr, uint64_t size, uint64_t flags) {
    return vmm_map_range(physical_addr, physical_addr, size, flags);
}

/* ── Page fault handler ──────────────────────────────────────────────────── */
void vmm_page_fault_handler(uint64_t error_code, uint64_t faulting_addr) {
    vmm_stats.page_faults++;

    if (demand_paging_enabled) {
        pte_t* pte = vmm_get_pte(kernel_address_space.pml4, faulting_addr, 0);
        if (pte && (*pte & PAGE_RESERVED) && (*pte & PAGE_ON_DEMAND)) {
            uint64_t flags = *pte & 0xFFF;
            flags &= ~PAGE_RESERVED;
            flags &= ~PAGE_ON_DEMAND;

            void* phys_frame = pmm_alloc_frame();
            if (phys_frame) {
                uint64_t page_aligned = VMM_PAGE_ALIGN_DOWN(faulting_addr);
                *pte = ((uint64_t)phys_frame & 0x000FFFFFFFFFF000ULL) |
                       flags | PAGE_PRESENT;
                vmm_flush_tlb_single(page_aligned);
                vmm_stats.demand_allocations++;
                vmm_stats.pages_mapped++;
                return;
            }
            serial_print("VMM: Out of memory — cannot allocate demanded page\n");
        }
    }

    serial_print("VMM: Unhandled page fault — error:");
    if (error_code & 0x1) serial_print(" PRESENT");
    if (error_code & 0x2) serial_print(" WRITE");
    if (error_code & 0x4) serial_print(" USER");
    if (error_code & 0x8) serial_print(" RESERVED");
    if (error_code & 0x10) serial_print(" INSTRUCTION");
    serial_print("\n");

    cpu_disable_interrupts();
    while (1) __asm__ volatile ("hlt");
}

/* ── Adres alanı yönetimi ────────────────────────────────────────────────── */
address_space_t* vmm_create_address_space(void) {
    address_space_t* space = (address_space_t*)kmalloc(sizeof(address_space_t));
    if (!space) return NULL;

    space->pml4 = vmm_alloc_page_table();
    if (!space->pml4) { kfree(space); return NULL; }

    /* Kernel eşlemelerini (PML4[256..511]) kopyala */
    for (int i = 256; i < 512; i++)
        space->pml4->entries[i] = kernel_address_space.pml4->entries[i];

    space->cr3_value = virt_to_phys((uint64_t)space->pml4);
    return space;
}

void vmm_destroy_address_space(address_space_t* space) {
    if (!space || space == &kernel_address_space) return;
    /* TODO: PML4[0..255] sayfa tablolarını özyinelemeli serbest bırak */
    vmm_free_page_table(space->pml4);
    kfree(space);
}

void vmm_switch_address_space(address_space_t* space) {
    if (!space) return;
    vmm_write_cr3(space->cr3_value);
}

address_space_t* vmm_get_kernel_space(void) { return &kernel_address_space; }

/* ── Demand paging ───────────────────────────────────────────────────────── */
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
int vmm_is_demand_paging_enabled(void) { return demand_paging_enabled; }

int vmm_allocate_on_demand(uint64_t virtual_addr, uint64_t flags) {
    if (!VMM_IS_PAGE_ALIGNED(virtual_addr)) return -1;
    pte_t* pte = vmm_get_pte(kernel_address_space.pml4, virtual_addr, 1);
    if (!pte) return -1;
    *pte = (flags & 0xFFF) | PAGE_RESERVED | PAGE_ON_DEMAND;
    vmm_stats.reserved_pages++;
    return 0;
}

int vmm_reserve_pages(uint64_t virtual_start, uint64_t count, uint64_t flags) {
    if (!VMM_IS_PAGE_ALIGNED(virtual_start)) return -1;
    for (uint64_t i = 0; i < count; i++) {
        if (vmm_allocate_on_demand(virtual_start + i * PAGE_SIZE_4K, flags) != 0)
            return -1;
    }
    return 0;
}

int vmm_commit_page(uint64_t virtual_addr) {
    if (!VMM_IS_PAGE_ALIGNED(virtual_addr)) return -1;
    pte_t* pte = vmm_get_pte(kernel_address_space.pml4, virtual_addr, 0);
    if (!pte || !(*pte & PAGE_RESERVED)) return -1;

    uint64_t flags = (*pte & 0xFFF) & ~PAGE_RESERVED & ~PAGE_ON_DEMAND;
    void* phys_frame = pmm_alloc_frame();
    if (!phys_frame) return -1;

    *pte = ((uint64_t)phys_frame & 0x000FFFFFFFFFF000ULL) | flags | PAGE_PRESENT;
    vmm_flush_tlb_single(virtual_addr);
    vmm_stats.pages_mapped++;
    vmm_stats.reserved_pages--;
    return 0;
}

int vmm_commit_range(uint64_t virtual_start, uint64_t count) {
    if (!VMM_IS_PAGE_ALIGNED(virtual_start)) return -1;
    for (uint64_t i = 0; i < count; i++) {
        if (vmm_commit_page(virtual_start + i * PAGE_SIZE_4K) != 0)
            return -1;
    }
    return 0;
}

/* ── Sayfa bilgisi ───────────────────────────────────────────────────────── */
int vmm_is_page_present(uint64_t virtual_addr) {
    pte_t* pte = vmm_get_pte(kernel_address_space.pml4, virtual_addr, 0);
    return (pte && (*pte & PAGE_PRESENT)) ? 1 : 0;
}

uint64_t vmm_get_page_flags(uint64_t virtual_addr) {
    pte_t* pte = vmm_get_pte(kernel_address_space.pml4, virtual_addr, 0);
    if (!pte) return 0;
    return *pte & 0xFFF;
}

void vmm_set_page_flags(uint64_t virtual_addr, uint64_t flags) {
    pte_t* pte = vmm_get_pte(kernel_address_space.pml4, virtual_addr, 0);
    if (!pte) return;
    *pte = PTE_GET_ADDR(*pte) | (flags & 0xFFF) | PAGE_PRESENT;
    vmm_flush_tlb_single(virtual_addr);
}

/* ── İstatistikler ───────────────────────────────────────────────────────── */
void vmm_print_stats(void) {
    serial_print("VMM Statistics:\n");
    serial_print("  Pages mapped/unmapped/faults/tlb — see vmm_get_* functions\n");
}

uint64_t vmm_get_pages_mapped(void)      { return vmm_stats.pages_mapped; }
uint64_t vmm_get_pages_unmapped(void)    { return vmm_stats.pages_unmapped; }
uint64_t vmm_get_page_faults(void)       { return vmm_stats.page_faults; }
uint64_t vmm_get_tlb_flushes(void)       { return vmm_stats.tlb_flushes; }
uint64_t vmm_get_demand_allocations(void){ return vmm_stats.demand_allocations; }
uint64_t vmm_get_reserved_pages(void)    { return vmm_stats.reserved_pages; }

void vmm_dump_page_tables(uint64_t virtual_addr) {
    serial_print("VMM: page table dump requested\n");
    (void)virtual_addr;
}