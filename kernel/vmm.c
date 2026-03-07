// vmm.c — Virtual Memory Manager
// 4 seviyeli sayfalama (PML4), higher-half kernel.
// Bağımlılık: pmm_alloc_frame / pmm_free_frame — heap yok, döngüsel bağımlılık yok.

#include "vmm.h"
#include "pmm.h"
#include <stdint.h>
#include <stddef.h>

// -----------------------------------------------------------------------
// Dış bildirimler
// -----------------------------------------------------------------------
extern void serial_print(const char *s);
extern void print_str64 (const char *s, uint8_t color);
extern void println64   (const char *s, uint8_t color);
extern void *memset64   (void *dst, int c, size_t n);
extern void *memcpy64   (void *dst, const void *src, size_t n);

#define CLR_GREEN   0x0A
#define CLR_YELLOW  0x0E
#define CLR_RED     0x0C
#define CLR_CYAN    0x03
#define CLR_WHITE   0x0F

// -----------------------------------------------------------------------
// Yardımcı: uint64_t → hex string
// -----------------------------------------------------------------------
static void u64_to_hex(uint64_t n, char *buf) {
    const char *h = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) buf[2 + i] = h[(n >> (60 - i * 4)) & 0xF];
    buf[18] = '\0';
}
static void u64_to_dec(uint64_t n, char *buf, int sz) {
    if (sz < 2) return;
    if (n == 0) { buf[0]='0'; buf[1]='\0'; return; }
    int i = 0; char tmp[24];
    while (n && i < (int)sizeof(tmp)-1) { tmp[i++]='0'+(n%10); n/=10; }
    int o = 0;
    while (i-- && o < sz-1) buf[o++] = tmp[i];
    buf[o] = '\0';
}

// -----------------------------------------------------------------------
// CR3 yardımcıları
// -----------------------------------------------------------------------
static inline uint64_t cr3_read(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline void cr3_write(uint64_t v) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(v) : "memory");
}

// -----------------------------------------------------------------------
// TLB
// -----------------------------------------------------------------------
void vmm_tlb_flush_page(uint64_t virt) {
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}
void vmm_tlb_flush_all(void) {
    cr3_write(cr3_read());
}

// -----------------------------------------------------------------------
// Sayfa tablosu tahsisi
// NOT: vmm_alloc_page_table, PMM'den doğrudan frame alır.
//      Heap henüz hazır olmayabilir (bootstrap sırası: PMM → VMM → Heap).
// -----------------------------------------------------------------------
page_table_t *vmm_alloc_page_table(void) {
    uint64_t phys = pmm_alloc_frame();
    if (!phys) {
        serial_print("[VMM] HATA: Sayfa tablosu icin frame alinamiyor\n");
        return NULL;
    }
    page_table_t *pt = (page_table_t *)PHYS_TO_VIRT(phys);
    memset64(pt, 0, sizeof(page_table_t));
    return pt;
}

void vmm_free_page_table(page_table_t *pt) {
    if (!pt) return;
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)pt));
}

// -----------------------------------------------------------------------
// Durum
// -----------------------------------------------------------------------
static address_space_t s_kernel_space;
static int             s_demand_paging = 0;

static vmm_stats_t s_stats;

// -----------------------------------------------------------------------
// Dahili: PTE işaretçisi al / oluştur
// create=1 → eksik ara tablolar PMM'den tahsis edilir
// -----------------------------------------------------------------------
static pte_t *get_pte(page_table_t *pml4, uint64_t virt, int create) {
    if (!pml4) return NULL;

    // --- PML4 ---
    pte_t *pml4e = &pml4->entries[PML4_IDX(virt)];
    page_table_t *pdpt;
    if (!(*pml4e & PTE_PRESENT)) {
        if (!create) return NULL;
        pdpt = vmm_alloc_page_table();
        if (!pdpt) return NULL;
        *pml4e = VIRT_TO_PHYS((uint64_t)pdpt) | PTE_PRESENT | PTE_WRITE;
    } else {
        pdpt = (page_table_t *)PHYS_TO_VIRT(PTE_ADDR(*pml4e));
    }

    // --- PDPT ---
    pte_t *pdpte = &pdpt->entries[PDPT_IDX(virt)];
    page_table_t *pd;
    if (!(*pdpte & PTE_PRESENT)) {
        if (!create) return NULL;
        pd = vmm_alloc_page_table();
        if (!pd) return NULL;
        *pdpte = VIRT_TO_PHYS((uint64_t)pd) | PTE_PRESENT | PTE_WRITE;
    } else {
        if (*pdpte & PTE_HUGE) return NULL;  // 1GB sayfa, 4KB PTE yok
        pd = (page_table_t *)PHYS_TO_VIRT(PTE_ADDR(*pdpte));
    }

    // --- PD ---
    pte_t *pde = &pd->entries[PD_IDX(virt)];
    page_table_t *pt;
    if (!(*pde & PTE_PRESENT)) {
        if (!create) return NULL;
        pt = vmm_alloc_page_table();
        if (!pt) return NULL;
        *pde = VIRT_TO_PHYS((uint64_t)pt) | PTE_PRESENT | PTE_WRITE;
    } else {
        if (*pde & PTE_HUGE) return NULL;    // 2MB sayfa
        pt = (page_table_t *)PHYS_TO_VIRT(PTE_ADDR(*pde));
    }

    // --- PT ---
    return &pt->entries[PT_IDX(virt)];
}

// -----------------------------------------------------------------------
// vmm_init
// -----------------------------------------------------------------------
void vmm_init(void) {
    serial_print("[VMM] Baslatiliyor...\n");

    uint64_t cr3 = cr3_read();
    s_kernel_space.cr3  = cr3;
    s_kernel_space.pml4 = (page_table_t *)PHYS_TO_VIRT(cr3);

    memset64(&s_stats, 0, sizeof(s_stats));

    serial_print("[VMM] Mevcut sayfa tablolari devralindi\n");
    serial_print("[VMM] Baslatma tamamlandi\n");
}

// -----------------------------------------------------------------------
// vmm_map_page
// -----------------------------------------------------------------------
int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!IS_PAGE_ALIGNED(virt) || !IS_PAGE_ALIGNED(phys)) return -1;

    pte_t *pte = get_pte(s_kernel_space.pml4, virt, 1);
    if (!pte) return -1;

    *pte = (phys & 0x000FFFFFFFFFF000ULL) | (flags | PTE_PRESENT);
    vmm_tlb_flush_page(virt);
    s_stats.pages_mapped++;
    return 0;
}

// -----------------------------------------------------------------------
// vmm_map_page_2m
// -----------------------------------------------------------------------
int vmm_map_page_2m(uint64_t virt, uint64_t phys, uint64_t flags) {
    if ((virt & (PAGE_SIZE_2M - 1)) || (phys & (PAGE_SIZE_2M - 1))) return -1;

    page_table_t *pml4 = s_kernel_space.pml4;

    // PML4
    pte_t *pml4e = &pml4->entries[PML4_IDX(virt)];
    page_table_t *pdpt;
    if (!(*pml4e & PTE_PRESENT)) {
        pdpt = vmm_alloc_page_table();
        if (!pdpt) return -1;
        *pml4e = VIRT_TO_PHYS((uint64_t)pdpt) | PTE_PRESENT | PTE_WRITE;
    } else {
        pdpt = (page_table_t *)PHYS_TO_VIRT(PTE_ADDR(*pml4e));
    }

    // PDPT
    pte_t *pdpte = &pdpt->entries[PDPT_IDX(virt)];
    page_table_t *pd;
    if (!(*pdpte & PTE_PRESENT)) {
        pd = vmm_alloc_page_table();
        if (!pd) return -1;
        *pdpte = VIRT_TO_PHYS((uint64_t)pd) | PTE_PRESENT | PTE_WRITE;
    } else {
        pd = (page_table_t *)PHYS_TO_VIRT(PTE_ADDR(*pdpte));
    }

    // PD — büyük sayfa olarak işaretle
    pd->entries[PD_IDX(virt)] = (phys & 0x000FFFFFFFE00000ULL)
                                 | flags | PTE_PRESENT | PTE_HUGE;
    vmm_tlb_flush_page(virt);
    s_stats.pages_mapped++;
    return 0;
}

// -----------------------------------------------------------------------
// vmm_unmap_page
// -----------------------------------------------------------------------
int vmm_unmap_page(uint64_t virt) {
    if (!IS_PAGE_ALIGNED(virt)) return -1;

    pte_t *pte = get_pte(s_kernel_space.pml4, virt, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return -1;

    *pte = 0;
    vmm_tlb_flush_page(virt);
    s_stats.pages_unmapped++;
    return 0;
}

// -----------------------------------------------------------------------
// vmm_map_range / vmm_unmap_range
// -----------------------------------------------------------------------
int vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) {
    virt = PAGE_ALIGN_DOWN(virt);
    phys = PAGE_ALIGN_DOWN(phys);
    size = PAGE_ALIGN_UP(size);
    for (uint64_t off = 0; off < size; off += PAGE_SIZE_4K)
        if (vmm_map_page(virt + off, phys + off, flags) != 0) return -1;
    return 0;
}

int vmm_unmap_range(uint64_t virt, uint64_t size) {
    virt = PAGE_ALIGN_DOWN(virt);
    size = PAGE_ALIGN_UP(size);
    for (uint64_t off = 0; off < size; off += PAGE_SIZE_4K)
        vmm_unmap_page(virt + off);
    return 0;
}

// -----------------------------------------------------------------------
// vmm_virt_to_phys
// Hem 4KB hem 2MB (huge) hem 1GB (huge) sayfaları destekler.
// Kernel kodu genellikle 2MB huge page ile eşlenir; eski sürüm
// PTE_HUGE gördüğünde NULL dönüp 0 veriyordu. Düzeltildi.
// -----------------------------------------------------------------------
uint64_t vmm_virt_to_phys(uint64_t virt) {
    if (!s_kernel_space.pml4) return 0;

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    pte_t pml4e = s_kernel_space.pml4->entries[pml4_idx];
    if (!(pml4e & PTE_PRESENT)) return 0;

    page_table_t *pdpt = (page_table_t *)PHYS_TO_VIRT(PTE_ADDR(pml4e));
    pte_t pdpte = pdpt->entries[pdpt_idx];
    if (!(pdpte & PTE_PRESENT)) return 0;

    /* 1GB huge page */
    if (pdpte & PTE_HUGE) {
        uint64_t base = PTE_ADDR(pdpte) & ~((1ULL << 30) - 1);
        return base | (virt & ((1ULL << 30) - 1));
    }

    page_table_t *pd = (page_table_t *)PHYS_TO_VIRT(PTE_ADDR(pdpte));
    pte_t pde = pd->entries[pd_idx];
    if (!(pde & PTE_PRESENT)) return 0;

    /* 2MB huge page — kernel kodu buraya eşlenir */
    if (pde & PTE_HUGE) {
        uint64_t base = PTE_ADDR(pde) & ~((1ULL << 21) - 1);
        return base | (virt & ((1ULL << 21) - 1));
    }

    page_table_t *pt = (page_table_t *)PHYS_TO_VIRT(PTE_ADDR(pde));
    pte_t pte = pt->entries[pt_idx];
    if (!(pte & PTE_PRESENT)) return 0;

    return PTE_ADDR(pte) | (virt & (PAGE_SIZE_4K - 1));
}

// -----------------------------------------------------------------------
// Adres uzayı yönetimi
// -----------------------------------------------------------------------
address_space_t *vmm_create_address_space(void) {
    // Heap bu noktada hazır olmalı; sadece boot sonrasında çağrılır.
    // kmalloc'u doğrudan çağırmak yerine bir PMM frame'i kullan
    // (address_space_t küçük, bir frame'e sığar).
    uint64_t phys = pmm_alloc_frame();
    if (!phys) return NULL;

    address_space_t *as = (address_space_t *)PHYS_TO_VIRT(phys);
    as->pml4 = vmm_alloc_page_table();
    if (!as->pml4) { pmm_free_frame(phys); return NULL; }

    // Çekirdek yüksek yarısını (256-511) kopyala
    for (int i = 256; i < 512; i++)
        as->pml4->entries[i] = s_kernel_space.pml4->entries[i];

    as->cr3 = VIRT_TO_PHYS((uint64_t)as->pml4);
    return as;
}

void vmm_destroy_address_space(address_space_t *as) {
    if (!as || as == &s_kernel_space) return;
    // Kullanıcı alanı (0-255) sayfa tablolarını özyinelemeli serbest bırak
    for (int i = 0; i < 256; i++) {
        pte_t pml4e = as->pml4->entries[i];
        if (!(pml4e & PTE_PRESENT)) continue;
        page_table_t *pdpt = (page_table_t *)PHYS_TO_VIRT(PTE_ADDR(pml4e));
        for (int j = 0; j < 512; j++) {
            pte_t pdpte = pdpt->entries[j];
            if (!(pdpte & PTE_PRESENT) || (pdpte & PTE_HUGE)) continue;
            page_table_t *pd = (page_table_t *)PHYS_TO_VIRT(PTE_ADDR(pdpte));
            for (int k = 0; k < 512; k++) {
                pte_t pde = pd->entries[k];
                if (!(pde & PTE_PRESENT) || (pde & PTE_HUGE)) continue;
                pmm_free_frame(PTE_ADDR(pde));  // PT frame'ini serbest bırak
            }
            vmm_free_page_table(pd);
        }
        vmm_free_page_table(pdpt);
    }
    vmm_free_page_table(as->pml4);
    pmm_free_frame(VIRT_TO_PHYS((uint64_t)as));
}

void vmm_switch_address_space(address_space_t *as) {
    if (!as) return;
    cr3_write(as->cr3);
}

address_space_t *vmm_kernel_space(void) {
    return &s_kernel_space;
}

// -----------------------------------------------------------------------
// Talep sayfalama
// -----------------------------------------------------------------------
void vmm_enable_demand_paging(void)  { s_demand_paging = 1; serial_print("[VMM] Talep sayfalama ACIK\n"); }
void vmm_disable_demand_paging(void) { s_demand_paging = 0; serial_print("[VMM] Talep sayfalama KAPALI\n"); }
int  vmm_is_demand_paging(void)      { return s_demand_paging; }

int vmm_reserve_page(uint64_t virt, uint64_t flags) {
    if (!IS_PAGE_ALIGNED(virt)) return -1;
    pte_t *pte = get_pte(s_kernel_space.pml4, virt, 1);
    if (!pte) return -1;
    // Fiziksel sayfa yok; sadece bayrakları ve DEMAND bitini kaydet
    *pte = (flags & 0xFFF & ~(uint64_t)PTE_PRESENT) | PTE_DEMAND;
    s_stats.pages_mapped++;  // sanal sayfa rezerve edildi
    return 0;
}

int vmm_reserve_range(uint64_t virt, uint64_t count, uint64_t flags) {
    for (uint64_t i = 0; i < count; i++)
        if (vmm_reserve_page(virt + i * PAGE_SIZE_4K, flags) != 0) return -1;
    return 0;
}

// -----------------------------------------------------------------------
// vmm_handle_page_fault
// -----------------------------------------------------------------------
void vmm_handle_page_fault(uint64_t error_code, uint64_t fault_addr) {
    s_stats.page_faults++;

    uint64_t page = PAGE_ALIGN_DOWN(fault_addr);
    pte_t *pte = get_pte(s_kernel_space.pml4, page, 0);

    // Talep sayfalama: DEMAND biti varsa fiziksel sayfa tahsis et
    if (pte && (*pte & PTE_DEMAND) && !(*pte & PTE_PRESENT)) {
        uint64_t flags = (*pte & 0xFFF) & ~(uint64_t)PTE_DEMAND;
        uint64_t phys  = pmm_alloc_frame();
        if (phys) {
            *pte = phys | flags | PTE_PRESENT;
            vmm_tlb_flush_page(page);
            s_stats.demand_allocs++;
            return;  // Hata işlendi
        }
        serial_print("[VMM] HATA: Talep sayfasi icin bellek yok!\n");
    }

    // İşlenemeyen sayfa hatası — çekirdeği durdur
    char hbuf[20];
    serial_print("[VMM] SAYFA HATASI! Adres: ");
    u64_to_hex(fault_addr, hbuf); serial_print(hbuf);
    serial_print("  Hata kodu: ");
    u64_to_hex(error_code, hbuf); serial_print(hbuf);
    serial_print("\n  [");
    if (error_code & 1)  serial_print("MEVCUT ");
    if (error_code & 2)  serial_print("YAZMA ");
    if (error_code & 4)  serial_print("KULLANICI ");
    if (error_code & 8)  serial_print("REZERVE ");
    if (error_code & 16) serial_print("FETCH ");
    serial_print("]\n");

    __asm__ volatile("cli; hlt");
}

// -----------------------------------------------------------------------
// İstatistikler
// -----------------------------------------------------------------------
vmm_stats_t vmm_get_stats(void) { return s_stats; }

void vmm_print_stats(void) {
    char buf[24];
    println64("=== VMM Istatistikleri ===", CLR_CYAN);

    print_str64("  Eslenen sayfalar    : ", CLR_WHITE);
    u64_to_dec(s_stats.pages_mapped,   buf, sizeof(buf)); println64(buf, CLR_GREEN);

    print_str64("  Kaldirilan sayfalar : ", CLR_WHITE);
    u64_to_dec(s_stats.pages_unmapped, buf, sizeof(buf)); println64(buf, CLR_YELLOW);

    print_str64("  Sayfa hatalari      : ", CLR_WHITE);
    u64_to_dec(s_stats.page_faults,    buf, sizeof(buf)); println64(buf, CLR_RED);

    print_str64("  Talep tahsisleri    : ", CLR_WHITE);
    u64_to_dec(s_stats.demand_allocs,  buf, sizeof(buf)); println64(buf, CLR_GREEN);

    print_str64("  TLB temizlemeleri   : ", CLR_WHITE);
    u64_to_dec(s_stats.tlb_flushes,    buf, sizeof(buf)); println64(buf, CLR_GREEN);
}

// -----------------------------------------------------------------------
// Geriye dönük uyumluluk takma adları
// -----------------------------------------------------------------------

// vmm_page_fault_handler: page_fault_handler.c'nin çağırdığı eski isim
void vmm_page_fault_handler(uint64_t error_code, uint64_t fault_addr) {
    vmm_handle_page_fault(error_code, fault_addr);
}

// vmm_get_physical_address: elf64.c ve diğer eski dosyaların çağırdığı isim
uint64_t vmm_get_physical_address(uint64_t virtual_addr) {
    return vmm_virt_to_phys(virtual_addr);
}