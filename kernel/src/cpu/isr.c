#include "isr.h"
#include "../console/console.h"

const char *exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",
    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved"
};

static void print_hex(uint64_t value) {
    const char *hex_chars = "0123456789ABCDEF";
    console_puts("0x");
    for (int i = 15; i >= 0; i--) {
        console_putchar(hex_chars[(value >> (i * 4)) & 0xF]);
    }
}

void isr_handler(struct registers *regs) {
    console_clear();
    console_puts("==================== KERNEL PANIC ====================\n");
    if (regs->int_no < 32) {
        console_puts(exception_messages[regs->int_no]);
        console_puts(" Exception\n");
    } else {
        console_puts("Unknown Exception\n");
    }
    
    console_puts("INT_NO: "); print_hex(regs->int_no); console_puts("\n");
    console_puts("ERR_CD: "); print_hex(regs->err_code); console_puts("\n\n");
    
    console_puts("RIP:    "); print_hex(regs->rip); console_puts("\n");
    console_puts("RSP:    "); print_hex(regs->rsp); console_puts("\n");
    console_puts("RFLAGS: "); print_hex(regs->rflags); console_puts("\n");
    console_puts("CS:     "); print_hex(regs->cs); console_puts("\n");

    if (regs->int_no == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r" (cr2));
        console_puts("\n=> PAGE FAULT ADDRESS (CR2): ");
        print_hex(cr2);
        console_puts("\n");
    }
    
    console_puts("\nSystem Halted.\n");
    
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
