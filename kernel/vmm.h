// vmm.h — Virtual Memory Manager (4-level paging, higher-half kernel)
#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

// -----------------------------------------------------------------------
// Sabit adresler
// -----------------------------------------------------------------------
#define KERNEL_VMA          0xFFFFFFFF80000000ULL   // Yüksek yarı offset
#define KERNEL_PHYS_BASE    0x100000ULL             // Çekirdeğin fiziksel başı

// Fiziksel ↔ sanal dönüşüm (boot'ta identity-map + higher-half map varsayılır)
#define PHYS_TO_VIRT(p)     ((uint64_t)(p) + KERNEL_VMA - KERNEL_PHYS_BASE)
#define VIRT_TO_PHYS(v)     ((uint64_t)(v) - KERNEL_VMA + KERNEL_PHYS_BASE)

// -----------------------------------------------------------------------
// PTE bayrakları
// -----------------------------------------------------------------------
#define PTE_PRESENT     (1ULL << 0)
#define PTE_WRITE       (1ULL << 1)
#define PTE_USER        (1ULL << 2)
#define PTE_PWT         (1ULL << 3)
#define PTE_PCD         (1ULL << 4)
#define PTE_ACCESSED    (1ULL << 5)
#define PTE_DIRTY       (1ULL << 6)
#define PTE_HUGE        (1ULL << 7)   // 2MB / 1GB sayfa
#define PTE_GLOBAL      (1ULL << 8)
#define PTE_NX          (1ULL << 63)

// Özel yazılım bitleri (9-11 arası CPU tarafından yok sayılır)
#define PTE_DEMAND      (1ULL << 9)   // Talep üzerine tahsis edilecek
#define PTE_COW         (1ULL << 10)  // Copy-on-write

// Bayrak kombinasyonları (sık kullanılanlar)
#define VMM_KERNEL_RW   (PTE_PRESENT | PTE_WRITE)
#define VMM_USER_RW     (PTE_PRESENT | PTE_WRITE | PTE_USER)
#define VMM_KERNEL_RO   (PTE_PRESENT)

// -----------------------------------------------------------------------
// Sayfa boyutları
// -----------------------------------------------------------------------
#define PAGE_SIZE_4K    4096ULL
#define PAGE_SIZE_2M    (2ULL * 1024 * 1024)
#define PAGE_SIZE_1G    (1024ULL * 1024 * 1024)

// -----------------------------------------------------------------------
// Adres yardımcıları
// -----------------------------------------------------------------------
#define PAGE_ALIGN_DOWN(a)  ((a) & ~(PAGE_SIZE_4K - 1))
#define PAGE_ALIGN_UP(a)    (((a) + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1))
#define IS_PAGE_ALIGNED(a)  (((a) & (PAGE_SIZE_4K - 1)) == 0)

// Sayfa tablosu indeksleri
#define PML4_IDX(a)  (((uint64_t)(a) >> 39) & 0x1FF)
#define PDPT_IDX(a)  (((uint64_t)(a) >> 30) & 0x1FF)
#define PD_IDX(a)    (((uint64_t)(a) >> 21) & 0x1FF)
#define PT_IDX(a)    (((uint64_t)(a) >> 12) & 0x1FF)

// PTE'den fiziksel adres çıkar
#define PTE_ADDR(pte)   ((pte) & 0x000FFFFFFFFFF000ULL)

// -----------------------------------------------------------------------
// Türler
// -----------------------------------------------------------------------
typedef uint64_t pte_t;

typedef struct {
    pte_t entries[512];
} __attribute__((aligned(4096))) page_table_t;

typedef struct {
    page_table_t *pml4;     // Sanal adres (PHYS_TO_VIRT ile erişilir)
    uint64_t      cr3;      // Fiziksel adres (CR3'e yüklenen)
} address_space_t;

// -----------------------------------------------------------------------
// Başlatma
// -----------------------------------------------------------------------

// Boot'ta kurulan mevcut sayfa tablolarını devral.
void vmm_init(void);

// -----------------------------------------------------------------------
// Eşleme işlemleri (kernel adres uzayı)
// -----------------------------------------------------------------------

// Tek 4KB sayfa eşle.  0 = başarılı, -1 = hata.
int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

// Tek 2MB büyük sayfa eşle.
int vmm_map_page_2m(uint64_t virt, uint64_t phys, uint64_t flags);

// Sayfa eşlemesini kaldır.
int vmm_unmap_page(uint64_t virt);

// Aralık eşle / kaldır.
int vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);
int vmm_unmap_range(uint64_t virt, uint64_t size);

// Fiziksel adres sorgula (0 = eşlenmemiş).
uint64_t vmm_virt_to_phys(uint64_t virt);

// -----------------------------------------------------------------------
// Adres uzayı yönetimi
// -----------------------------------------------------------------------
address_space_t *vmm_create_address_space(void);
void             vmm_destroy_address_space(address_space_t *as);
void             vmm_switch_address_space(address_space_t *as);
address_space_t *vmm_kernel_space(void);

// -----------------------------------------------------------------------
// Talep sayfalama (demand paging)
// -----------------------------------------------------------------------
void vmm_enable_demand_paging(void);
void vmm_disable_demand_paging(void);
int  vmm_is_demand_paging(void);

// Sanal adrese talep girişi yaz (fiziksel sayfa henüz yok).
int vmm_reserve_page(uint64_t virt, uint64_t flags);
int vmm_reserve_range(uint64_t virt, uint64_t count, uint64_t flags);

// Sayfa hatasını işle (ISR'dan çağrılır).
void vmm_handle_page_fault(uint64_t error_code, uint64_t fault_addr);

// -----------------------------------------------------------------------
// TLB
// -----------------------------------------------------------------------
void vmm_tlb_flush_page(uint64_t virt);
void vmm_tlb_flush_all(void);

// -----------------------------------------------------------------------
// Sayfa tablosu yardımcısı (PMM bootstrap için doğrudan kullanılır)
// -----------------------------------------------------------------------
page_table_t *vmm_alloc_page_table(void); // PMM'den bir frame alır
void          vmm_free_page_table(page_table_t *pt);

// -----------------------------------------------------------------------
// İstatistikler
// -----------------------------------------------------------------------
typedef struct {
    uint64_t pages_mapped;
    uint64_t pages_unmapped;
    uint64_t page_faults;
    uint64_t demand_allocs;
    uint64_t tlb_flushes;
} vmm_stats_t;

vmm_stats_t vmm_get_stats(void);
void        vmm_print_stats(void);

#endif // VMM_H