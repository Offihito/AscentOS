// memory_unified.h — Geriye Dönük Uyumluluk Köprüsü
//
// Eski kod bu başlığı include etmeye devam edebilir.
// Yeni kod doğrudan pmm.h / vmm.h / heap.h kullanmalıdır.
//
// ÖNEMLI: Bu dosya vmm64.h ile birlikte include edilmemelidir.
//         vmm64.h'ı kaldırın; yeni vmm.h tüm işlevleri sağlar.

#ifndef MEMORY_UNIFIED_H
#define MEMORY_UNIFIED_H

// vmm64.h daha önce include edilmişse veya ileride edilecekse
// include guard'ını şimdiden kapat — çakışmayı önler.
#ifndef VMM64_H
#define VMM64_H
#endif

#include <stdint.h>
#include <stddef.h>

// Yeni modüller
#include "pmm.h"
#include "vmm.h"
#include "heap.h"

// -----------------------------------------------------------------------
// Eski PMM struct uyumluluğu
// pmm_mmap_entry_t ile aynı bellek düzeni — doğrudan cast edilebilir.
// -----------------------------------------------------------------------
#ifndef MEMORY_MAP_ENTRY_DEFINED
#define MEMORY_MAP_ENTRY_DEFINED
struct memory_map_entry {
    unsigned long base;
    unsigned long length;
    unsigned int  type;
    unsigned int  acpi_extended;
} __attribute__((packed));
#endif

// -----------------------------------------------------------------------
// Eski VMM bayrak takma adları
// Yeni kod PTE_* kullanır; eski kod PAGE_* ile derlenmişse bunlar karşılar.
// -----------------------------------------------------------------------
#ifndef PAGE_PRESENT
#define PAGE_PRESENT    PTE_PRESENT
#endif
#ifndef PAGE_WRITE
#define PAGE_WRITE      PTE_WRITE
#endif
#ifndef PAGE_USER
#define PAGE_USER       PTE_USER
#endif
#ifndef PAGE_NX
#define PAGE_NX         PTE_NX
#endif

// -----------------------------------------------------------------------
// Eski fonksiyon ismi takma adları (inline → sıfır maliyet)
// -----------------------------------------------------------------------

// vmm_get_physical_address → vmm_virt_to_phys
static inline uint64_t vmm_get_physical_address(uint64_t virt) {
    return vmm_virt_to_phys(virt);
}

// vmm_identity_map → vmm_map_range(virt, virt, size, flags)
static inline int vmm_identity_map(uint64_t addr, uint64_t size, uint64_t flags) {
    return vmm_map_range(addr, addr, size, flags);
}

// vmm_map_page_2mb → vmm_map_page_2m (isim değişti)
static inline int vmm_map_page_2mb(uint64_t virt, uint64_t phys, uint64_t flags) {
    return vmm_map_page_2m(virt, phys, flags);
}

// vmm_is_page_present → vmm_virt_to_phys != 0
static inline int vmm_is_page_present(uint64_t virt) {
    return vmm_virt_to_phys(virt) != 0;
}

// vmm_is_demand_paging_enabled → vmm_is_demand_paging
static inline int vmm_is_demand_paging_enabled(void) {
    return vmm_is_demand_paging();
}

// vmm_reserve_pages → vmm_reserve_range
static inline int vmm_reserve_pages(uint64_t virt, uint64_t count, uint64_t flags) {
    return vmm_reserve_range(virt, count, flags);
}

// Eski stat getters → vmm_get_stats() üzerinden
static inline uint64_t vmm_get_pages_mapped(void)       { return vmm_get_stats().pages_mapped;   }
static inline uint64_t vmm_get_pages_unmapped(void)     { return vmm_get_stats().pages_unmapped; }
static inline uint64_t vmm_get_page_faults(void)        { return vmm_get_stats().page_faults;    }
static inline uint64_t vmm_get_tlb_flushes(void)        { return vmm_get_stats().tlb_flushes;    }
static inline uint64_t vmm_get_demand_allocations(void) { return vmm_get_stats().demand_allocs;  }
static inline uint64_t vmm_get_reserved_pages(void)     { return 0; }

// -----------------------------------------------------------------------
// Diğer eski semboller
// -----------------------------------------------------------------------
static inline void *map_page(uint64_t physical, uint64_t virtual_addr) {
    vmm_map_page(virtual_addr, physical, VMM_KERNEL_RW);
    return (void *)virtual_addr;
}

static inline void     set_static_heap_mode(int e)   { (void)e; }
static inline uint64_t get_total_memory(void)         { return pmm_total_bytes(); }

// pmm_alloc_pages / pmm_free_pages — kullanımdan kaldırıldı
static inline void *pmm_alloc_pages(uint64_t c)                   { (void)c; return NULL; }
static inline void  pmm_free_pages(void *b, uint64_t c)           { (void)b; (void)c; }
static inline void *pmm_alloc_pages_flags(uint64_t c, uint64_t f) { (void)c; (void)f; return NULL; }

// Heap pointer uyumluluğu
extern uint8_t *heap_start;
extern uint8_t *heap_current;

// Geriye dönük init takma adları (heap.c içinde tanımlı)
void init_memory_unified(void);
void init_memory64(void);
void init_memory_gui(void);

#endif // MEMORY_UNIFIED_H