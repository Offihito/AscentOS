#ifndef IDT64_H
#define IDT64_H

#include <stdint.h>

struct idt_entry {
    uint16_t offset_low;   
    uint16_t selector;     
    uint8_t  ist;         
    uint8_t  type_attr;    
    uint16_t offset_mid;   
    uint32_t offset_high;  
    uint32_t reserved;     
} __attribute__((packed));

// IDTR (IDT Register) Structure
struct idt_ptr {
    uint16_t limit;   
    uint64_t base;    
} __attribute__((packed));

// Public API
void init_interrupts64(void);

void idt_set_entry(int n, uint64_t handler, uint16_t selector, uint8_t attr);

void idt_irq_enable(uint8_t irq);

void idt_irq_disable(uint8_t irq);

#endif 