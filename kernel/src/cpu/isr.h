#ifndef CPU_ISR_H
#define CPU_ISR_H

#include <stdint.h>

struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*isr_t)(struct registers *);
void register_interrupt_handler(uint8_t n, isr_t handler);

void isr_handler(struct registers *regs);

#endif
