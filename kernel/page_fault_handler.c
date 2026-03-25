#include <stdint.h>
#include "vmm64.h"

extern void serial_print(const char* str);

void page_fault_handler_wrapper(void) {
    uint64_t faulting_addr;
    uint64_t error_code;
    
    __asm__ volatile ("mov %%cr2, %0" : "=r"(faulting_addr));

    __asm__ volatile ("mov 8(%%rsp), %0" : "=r"(error_code));
    
    vmm_page_fault_handler(error_code, faulting_addr);
}

uint64_t get_cr2(void) {
    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}