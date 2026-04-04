#include "gdt.h"

static struct gdt_entry gdt[3];
static struct gdt_ptr gp;

extern void gdt_flush(uint64_t);

static void gdt_set_gate(int num, uint64_t base, uint64_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;

    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F);

    gdt[num].granularity |= (gran & 0xF0);
    gdt[num].access      = access;
}

void gdt_init(void) {
    gp.limit = (sizeof(struct gdt_entry) * 3) - 1;
    gp.base  = (uint64_t)&gdt;

    // 0: Null descriptor
    gdt_set_gate(0, 0, 0, 0, 0);
    
    // 1: Kernel Code descriptor
    // Access: Present(1) Ring0(00) Type(1) Exec(1) Dir/Conf(0) R/W(1) Accessed(0) => 0x9A
    // Flags: Granularity(1) 32-bit(0) 64-bit(1) AVL(0) => 0xA0
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xAF);

    // 2: Kernel Data descriptor
    // Access: Present(1) Ring0(00) Type(1) Exec(0) Dir/Conf(0) R/W(1) Accessed(0) => 0x92
    // Flags: Granularity(1) 32-bit(1) 64-bit(0) AVL(0) => 0xC0
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    gdt_flush((uint64_t)&gp);
}

void gdt_load_ap(void) {
    // Load the already built main GDT for Application Processors.
    gdt_flush((uint64_t)&gp);
}
