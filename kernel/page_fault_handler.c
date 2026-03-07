// page_fault_handler.c - Page Fault Handler Wrapper
#include <stdint.h>
#include "vmm64.h"

// External serial print
extern void serial_print(const char* str);

// Page fault handler called from ISR
void page_fault_handler_wrapper(void) {
    uint64_t faulting_addr;
    uint64_t error_code;
    
    // Get CR2 (faulting address)
    __asm__ volatile ("mov %%cr2, %0" : "=r"(faulting_addr));
    
    // Error code is pushed by CPU, retrieve from stack
    // This assumes standard interrupt frame
    __asm__ volatile ("mov 8(%%rsp), %0" : "=r"(error_code));
    
    // Call VMM page fault handler
    vmm_page_fault_handler(error_code, faulting_addr);
}

// Helper to get CR2
uint64_t get_cr2(void) {
    uint64_t cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}