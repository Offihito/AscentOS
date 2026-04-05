#include "gdt.h"
#include "../lib/string.h"

// 0: Null, 1: KCode, 2: KData, 3: UData, 4: UCode, 5: TSS (Low), 6: TSS (High)
static struct gdt_entry gdt[7];
static struct gdt_ptr gp;
static struct tss_entry tss_bsp;

extern void gdt_flush(uint64_t);

static inline void ltr(uint16_t sel) {
    __asm__ volatile("ltr %0" : : "r"(sel));
}

static void gdt_set_gate(int num, uint64_t base, uint64_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;

    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F);

    gdt[num].granularity |= (gran & 0xF0);
    gdt[num].access      = access;
}

static void gdt_set_tss(int num, uint64_t base, uint32_t limit) {
    gdt_set_gate(num, base, limit, 0x89, 0x00);
    gdt[num + 1].limit_low = (uint16_t)(base >> 32);
    gdt[num + 1].base_low  = (uint16_t)(base >> 48);
    gdt[num + 1].base_middle = 0;
    gdt[num + 1].access = 0;
    gdt[num + 1].granularity = 0;
    gdt[num + 1].base_high = 0;
}

void tss_set_rsp0(uint64_t rsp0) {
    tss_bsp.rsp0 = rsp0;
}

void gdt_init(void) {
    gp.limit = (sizeof(struct gdt_entry) * 7) - 1;
    gp.base  = (uint64_t)&gdt;

    // 0: Null descriptor
    gdt_set_gate(0, 0, 0, 0, 0);
    
    // 1: Kernel Code descriptor (0x08)
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xAF);

    // 2: Kernel Data descriptor (0x10)
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    // 3: User Data descriptor (0x1B)   - DPL 3, Data R/W
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xF2, 0xC0); // F2 = 1 11 1 0 0 1 0 (Pr, DPL=3, S=1, C=0, E=0, W=1, A=0)

    // 4: User Code descriptor (0x23)   - DPL 3, Code Exec/Read, 64-bit
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xFA, 0xAF); // FA = 1 11 1 1 0 1 0 (Pr, DPL=3, S=1, C=1, C=0, R=1, A=0)

    // 5-6: TSS descriptor (0x28)
    memset(&tss_bsp, 0, sizeof(struct tss_entry));
    tss_bsp.iopb_offset = sizeof(struct tss_entry);
    gdt_set_tss(5, (uint64_t)&tss_bsp, sizeof(struct tss_entry) - 1);

    gdt_flush((uint64_t)&gp);
    ltr(0x28);
}

void gdt_load_ap(void) {
    gdt_flush((uint64_t)&gp);
}
