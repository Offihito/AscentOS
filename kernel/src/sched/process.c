#include "../console/klog.h"
#include "../cpu/gdt.h"
#include "../cpu/msr.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../smp/cpu.h"
#include "../syscalls/syscall.h"
#include "elf.h"
#include "sched.h"

// ── Kernel setjmp/longjmp for returning from userspace ──────────────────────
// jmp_buf: rbx, rbp, r12, r13, r14, r15, rsp, rip  (8 x uint64_t)
typedef uint64_t kernel_jmp_buf[8];

extern int kernel_setjmp(kernel_jmp_buf buf);
extern void kernel_longjmp(kernel_jmp_buf buf, int val)
    __attribute__((noreturn));

// Global context buffer: sys_exit will longjmp here to return to shell.
kernel_jmp_buf process_return_ctx;

// ── Memory management state for the current process ─────────────────────────
uint64_t process_brk_base    = 0;  // End of last PT_LOAD (initial brk)
uint64_t process_brk_current = 0;  // Current program break

extern void mm_reset_mmap_state(void);

// MSR constants for TLS base registers
#define IA32_KERNEL_GS_BASE 0xC0000102
#define IA32_FS_BASE        0xC0000100

// Inline assembly to perform sysret transition.
// Note: sysret loads CS from STAR MSR and SS from STAR MSR + 8.
// We must clear unnecessary registers to prevent info leaks.
static __attribute__((noreturn)) void process_jump_usermode(uint64_t rip,
                                                            uint64_t user_rsp) {
  // Reset user TLS to 0
  wrmsr(IA32_KERNEL_GS_BASE, 0);
  wrmsr(IA32_FS_BASE, 0);

  register uint64_t asm_rip asm("rcx") = rip;
  register uint64_t asm_rflags asm("r11") = 0x202;
  register uint64_t asm_rsp asm("rdi") =
      user_rsp; // Use rdi temporarily to hold stack

  __asm__ volatile(".intel_syntax noprefix\n"
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

                   "swapgs\n" // Put user's GS base (0) into active GS, and save
                              // kernel GS base

                   // Jump to user space!
                   "sysretq\n"
                   ".att_syntax prefix\n"
                   :
                   : "c"(asm_rip), "r"(asm_rflags), [usr_stack] "r"(asm_rsp)
                   : "memory");
  while (1)
    ;
}

#define PAGE_SIZE 4096

bool elf_load(const char *path, uint64_t *pml4, uint64_t *out_entry) {
  // Reset brk state for this new process
  process_brk_base = 0;
  process_brk_current = 0;
  
  vfs_node_t *file = vfs_resolve_path(path);
  if (!file) {
    klog_puts("[PROC] ELF load failed: File not found.\n");
    return false;
  }

  // Read the ELF Header
  Elf64_Ehdr ehdr;
  if (vfs_read(file, 0, sizeof(Elf64_Ehdr), (uint8_t *)&ehdr) !=
      sizeof(Elf64_Ehdr)) {
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
    klog_puts("[PROC] ELF load failed: Not a 64-bit ELF.\n");
    return false;
  }

  // Read Program Headers
  for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
    Elf64_Phdr phdr;
    uint32_t offset = ehdr.e_phoff + (i * ehdr.e_phentsize);

    if (vfs_read(file, offset, sizeof(Elf64_Phdr), (uint8_t *)&phdr) !=
        sizeof(Elf64_Phdr)) {
      continue;
    }

    if (phdr.p_type == PT_LOAD) {
      uint64_t memsz = phdr.p_memsz;
      uint64_t filesz = phdr.p_filesz;
      uint64_t vaddr = phdr.p_vaddr;
      uint64_t file_offset = phdr.p_offset;

      // Track uppermost loaded address for brk base.
      uint64_t seg_end = vaddr + memsz;
      if (seg_end > process_brk_base) {
          klog_puts("[PROC] PT_LOAD segment: vaddr=");
          klog_uint64(vaddr);
          klog_puts(" memsz=");
          klog_uint64(memsz);
          klog_puts(" seg_end=");
          klog_uint64(seg_end);
          klog_puts("\n");
          process_brk_base = seg_end;
      }

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
        if (!vmm_map_page(pml4, page, (uint64_t)phys,
                         PAGE_FLAG_USER | PAGE_FLAG_RW | PAGE_FLAG_PRESENT)) {
            klog_puts("[PROC] Exec failed: vmm_map_page failed\n");
            return false;
        }

        // Zero via kernel-accessible HHDM address, not user virtual address
        void *kernel_virt = (void *)((uint64_t)phys + pmm_get_hhdm_offset());
        memset(kernel_virt, 0, PAGE_SIZE);
      }

      // Read the binary data into the mapped segment
      if (filesz > 0) {
        uint32_t bytes_read = vfs_read(file, file_offset, filesz, (uint8_t *)vaddr);
        if (bytes_read != filesz) {
            klog_puts("[PROC] Exec failed: vfs_read short read (got ");
            klog_uint64(bytes_read);
            klog_puts(", expected ");
            klog_uint64(filesz);
            klog_puts(")\n");
            return false;
        }
      }
    }
  }

  // User stack: [stack_top - stack_size, stack_top); stack_top is unmapped (guard).
  uint64_t stack_top = ASCENTOS_USER_STACK_TOP;
  uint64_t stack_size = 4 * PAGE_SIZE; // 16 KB stack
  uint64_t stack_bottom = stack_top - stack_size;

  for (uint64_t page = stack_bottom; page < stack_top; page += PAGE_SIZE) {
    void *phys = pmm_alloc();
    if (!phys)
      return false;
    if (!vmm_map_page(pml4, page, (uint64_t)phys,
                     PAGE_FLAG_USER | PAGE_FLAG_RW | PAGE_FLAG_PRESENT)) {
        klog_puts("[PROC] Exec failed: vmm_map_page stack failed\n");
        return false;
    }
  }

  // Page-align the brk base upward and set current brk.
  klog_puts("[PROC] Before brk alignment: process_brk_base=");
  klog_uint64(process_brk_base);
  klog_puts("\n");
  process_brk_base = (process_brk_base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  klog_puts("[PROC] After brk alignment: process_brk_base=");
  klog_uint64(process_brk_base);
  klog_puts("\n");
  process_brk_current = process_brk_base;
    
  // Reset the mmap bump allocator for this new process.
  mm_reset_mmap_state();
    
  if (out_entry) *out_entry = ehdr.e_entry;

  return true;
}

uint64_t process_build_initial_stack(uint64_t stack_top, const char *path) {
  size_t path_len = strlen(path);
  uint64_t path_addr = stack_top - (path_len + 1);
  memcpy((void *)path_addr, path, path_len + 1);

  uint64_t *stack = (uint64_t *)path_addr;
  stack = (uint64_t *)((uint64_t)stack & ~0xFULL);

  // SysV x86_64 process entry: argc, argv..., NULL, envp..., NULL, auxv..., (0,0)
  *(--stack) = 0; // AT_NULL value
  *(--stack) = 0; // AT_NULL type
  *(--stack) = 0; // end envp
  *(--stack) = 0; // end argv
  *(--stack) = path_addr; // argv[0]
  *(--stack) = 1;          // argc

  return (uint64_t)stack;
}

bool process_exec(const char *path) {
  klog_puts("[PROC] Attempting to execute: ");
  klog_puts(path);
  klog_puts("\n");

  size_t path_len = strlen(path);
  char *path_copy = kmalloc(path_len + 1);
  if (!path_copy) {
    klog_puts("[PROC] exec: kmalloc path failed\n");
    return false;
  }
  memcpy(path_copy, path, path_len + 1);

  uint64_t *pml4 = vmm_get_active_pml4();
  uint64_t entry_point = 0;

  if (!elf_load(path_copy, pml4, &entry_point)) {
    kfree(path_copy);
    return false;
  }

  klog_puts("[PROC] ELF mapped successfully. Jumping to Ring 3 (RIP: ");
  klog_uint64(entry_point);
  klog_puts(")\n");

  uint64_t user_rsp =
      process_build_initial_stack(ASCENTOS_USER_STACK_TOP, path_copy);
  kfree(path_copy);

  // Initialize File Descriptors for the main thread
  struct thread *current_thread = sched_get_current();
  if (current_thread) {
      // 0 = stdin, 1 = stdout, 2 = stderr. 
      // We map them all to the system console for now.
      vfs_node_t *console_node = vfs_resolve_path("/dev/console");
      current_thread->fds[0] = console_node;
      current_thread->fds[1] = console_node;
      current_thread->fds[2] = console_node;
      
      for (int i = 3; i < MAX_FDS; i++) {
          current_thread->fds[i] = NULL;
      }
  }

  // Set the TSS rsp0 to the kernel stack for hardware interrupts
  tss_set_rsp0(cpu_get_current()->stack_top);

  // Save kernel context so sys_exit can return here
  if (kernel_setjmp(process_return_ctx) != 0) {
    // ── We've been longjmp'd back from sys_exit ──
    // Restore kernel data segments (syscall came from Ring 3)
    __asm__ volatile("mov $0x10, %%ax\n" // Kernel Data selector
                     "mov %%ax, %%ds\n"
                     "mov %%ax, %%es\n" ::
                         : "eax");
    // Re-enable interrupts (FMASK cleared IF on syscall entry)
    __asm__ volatile("sti");
    return true;
  }

  // Issue the jump to userspace (does not return normally)
  process_jump_usermode(entry_point, user_rsp);
}
