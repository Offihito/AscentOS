#include "sched.h"
#include "elf.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../mm/heap.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../console/klog.h"
#include "../smp/cpu.h"
#include "../cpu/gdt.h"

// ── Kernel setjmp/longjmp for returning from userspace ──────────────────────
// jmp_buf: rbx, rbp, r12, r13, r14, r15, rsp, rip  (8 x uint64_t)
typedef uint64_t kernel_jmp_buf[8];

extern int  kernel_setjmp(kernel_jmp_buf buf);
extern void kernel_longjmp(kernel_jmp_buf buf, int val) __attribute__((noreturn));

// Global context buffer: sys_exit will longjmp here to return to shell.
kernel_jmp_buf process_return_ctx;

// Inline assembly to perform sysret transition.
// Note: sysret loads CS from STAR MSR and SS from STAR MSR + 8.
// We must clear unnecessary registers to prevent info leaks.
static __attribute__((noreturn)) void process_jump_usermode(uint64_t rip, uint64_t user_rsp) {
    register uint64_t asm_rip asm("rcx") = rip;
    register uint64_t asm_rflags asm("r11") = 0x202;
    register uint64_t asm_rsp asm("rdi") = user_rsp; // Use rdi temporarily to hold stack

    __asm__ volatile(
        ".intel_syntax noprefix\n"
        "mov ax, 0x1B\n" 
        "mov ds, ax\n"
        "mov es, ax\n"
        "mov rsp, %[usr_stack]\n"
        
        // Clear remaining general-purpose registers
        "xor rax, rax\n"
        "xor rbx, rbx\n"
        "xor rdx, rdx\n"
        "xor rdi, rdi\n"
        "xor rsi, rsi\n"
        "xor r8, r8\n"
        "xor r9, r9\n"
        "xor r10, r10\n"
        "xor r12, r12\n"
        "xor r13, r13\n"
        "xor r14, r14\n"
        "xor r15, r15\n"
        "xor rbp, rbp\n"
        
        // Jump to user space!
        "sysretq\n"
        ".att_syntax prefix\n"
        : 
        : "c" (asm_rip), "r" (asm_rflags), [usr_stack] "r" (asm_rsp)
        : "memory"
    );
    while (1);
}

// Simple path resolver without modifying original string
static vfs_node_t* resolve_path(const char *path) {
    if (!path || *path != '/') return NULL;
    vfs_node_t *current = fs_root;
    
    char comp[128];
    const char *p = path + 1; // Skip initial slash
    
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < 127) {
            comp[i++] = *p++;
        }
        comp[i] = '\0';
        
        while (*p == '/') p++; // Skip extra slashes
        
        if (i == 0) continue;
        
        current = vfs_finddir(current, comp);
        if (!current) {
            klog_puts("[PROC] Path resolution failed at component: ");
            klog_puts(comp);
            klog_puts("\n");
            return NULL;
        }
    }
    return current;
}

#define PAGE_SIZE 4096

bool process_exec(const char *path) {
    klog_puts("[PROC] Attempting to load ELF: ");
    klog_puts(path);
    klog_puts("\n");
    
    vfs_node_t *file = resolve_path(path);
    if (!file) {
        klog_puts("[PROC] Exec failed: File not found.\n");
        return false;
    }
    
    // Read the ELF Header
    Elf64_Ehdr ehdr;
    if (vfs_read(file, 0, sizeof(Elf64_Ehdr), (uint8_t*)&ehdr) != sizeof(Elf64_Ehdr)) {
        klog_puts("[PROC] Exec failed: Could not read ELF header.\n");
        return false;
    }
    
    // Check magic
    if (ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' || 
        ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F') {
        klog_puts("[PROC] Exec failed: Invalid ELF magic.\n");
        return false;
    }
    
    // We only support 64-bit EXEs
    if (ehdr.e_ident[4] != 2) { // 2 = 64-bit
        klog_puts("[PROC] Exec failed: Not a 64-bit ELF.\n");
        return false;
    }
    
    uint64_t *pml4 = vmm_get_active_pml4();
    
    // Read Program Headers
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr phdr;
        uint32_t offset = ehdr.e_phoff + (i * ehdr.e_phentsize);
        
        if (vfs_read(file, offset, sizeof(Elf64_Phdr), (uint8_t*)&phdr) != sizeof(Elf64_Phdr)) {
            continue;
        }
        
        if (phdr.p_type == PT_LOAD) {
            uint64_t memsz = phdr.p_memsz;
            uint64_t filesz = phdr.p_filesz;
            uint64_t vaddr = phdr.p_vaddr;
            uint64_t file_offset = phdr.p_offset;
            
            // Align and map pages
            uint64_t start_page = vaddr & ~(PAGE_SIZE - 1);
            uint64_t end_page = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            
            for (uint64_t page = start_page; page < end_page; page += PAGE_SIZE) {
                void *phys = pmm_alloc();
                if (!phys) {
                    klog_puts("[PROC] Exec failed: OOM allocating PT_LOAD.\n");
                    return false;
                }
                
                // Add USER, RW and PRESENT flags
                vmm_map_page(pml4, page, (uint64_t)phys, PAGE_FLAG_USER | PAGE_FLAG_RW | PAGE_FLAG_PRESENT);
                
                // Zero the page initially
                memset((void*)page, 0, PAGE_SIZE);
            }
            
            // Read the binary data into the mapped segment
            if (filesz > 0) {
                vfs_read(file, file_offset, filesz, (uint8_t*)vaddr);
            }
        }
    }
    
    // Setup a User Stack at a high virtual address (e.g. 0x00007FFFF0000000)
    uint64_t stack_top = 0x00007FFFF0000000;
    uint64_t stack_size = 4 * PAGE_SIZE; // 16 KB stack
    uint64_t stack_bottom = stack_top - stack_size;
    
    for (uint64_t page = stack_bottom; page < stack_top; page += PAGE_SIZE) {
        void *phys = pmm_alloc();
        if (!phys) return false;
        vmm_map_page(pml4, page, (uint64_t)phys, PAGE_FLAG_USER | PAGE_FLAG_RW | PAGE_FLAG_PRESENT);
    }
    
    klog_puts("[PROC] ELF mapped successfully. Jumping to Ring 3 (RIP: ");
    klog_uint64(ehdr.e_entry);
    klog_puts(")\n");
    
    // Set the TSS rsp0 to the kernel stack for hardware interrupts
    tss_set_rsp0(cpu_get_current()->stack_top);
    
    // Save kernel context so sys_exit can return here
    if (kernel_setjmp(process_return_ctx) != 0) {
        // ── We've been longjmp'd back from sys_exit ──
        // Restore kernel data segments (syscall came from Ring 3)
        __asm__ volatile(
            "mov $0x10, %%ax\n"    // Kernel Data selector
            "mov %%ax, %%ds\n"
            "mov %%ax, %%es\n"
            ::: "eax"
        );
        // Re-enable interrupts (FMASK cleared IF on syscall entry)
        __asm__ volatile("sti");
        return true;
    }
    
    // Issue the jump to userspace (does not return normally)
    process_jump_usermode(ehdr.e_entry, stack_top);
}
