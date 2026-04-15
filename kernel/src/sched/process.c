#include "../console/console.h"
#include "../console/klog.h"
#include "../cpu/gdt.h"
#include "../cpu/msr.h"
#include "../fb/framebuffer.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../smp/cpu.h"
#include "../syscalls/syscall.h"
#include "elf.h"
#include "sched.h"

static void process_copy_to_user(uint64_t *pml4, uint64_t dest_user_va,
                                 const void *src_kern_va, size_t size) {
  uint64_t hhdm = pmm_get_hhdm_offset();
  size_t done = 0;
  while (done < size) {
    uint64_t va = dest_user_va + done;
    uint64_t offset = va % PAGE_SIZE;
    uint32_t to_copy = PAGE_SIZE - offset;
    if (to_copy > size - done)
      to_copy = size - done;

    uint64_t phys = vmm_virt_to_phys(pml4, va);
    if (phys != 0) {
      memcpy((void *)(phys + hhdm), (const uint8_t *)src_kern_va + done,
             to_copy);
    }
    done += to_copy;
  }
}

// ── Kernel setjmp/longjmp for returning from userspace ──────────────────────
// jmp_buf: rbx, rbp, r12, r13, r14, r15, rsp, rip  (8 x uint64_t)
typedef uint64_t kernel_jmp_buf[8];

extern int kernel_setjmp(kernel_jmp_buf buf);
extern void kernel_longjmp(kernel_jmp_buf buf, int val)
    __attribute__((noreturn));

// Global context buffer: sys_exit will longjmp here to return to shell.
kernel_jmp_buf process_return_ctx;

// ── Memory management state for the current process ─────────────────────────
uint64_t process_brk_base = 0;    // End of last PT_LOAD (initial brk)
uint64_t process_brk_current = 0; // Current program break

extern void mm_reset_mmap_state(void);

// MSR constants for TLS base registers
#define IA32_KERNEL_GS_BASE 0xC0000102
#define IA32_FS_BASE 0xC0000100

// Inline assembly to perform sysret transition.
// Note: sysret loads CS from STAR MSR and SS from STAR MSR + 8.
// We must clear unnecessary registers to prevent info leaks.
void process_jump_usermode(uint64_t rip, uint64_t user_rsp, uint64_t pml4) {
  uint64_t hhdm = pmm_get_hhdm_offset();

  // DEBUG: Hexdump the entry point
  uint64_t rip_phys = vmm_virt_to_phys((uint64_t *)pml4, rip);
  if (rip_phys) {
    uint8_t *code = (uint8_t *)(rip_phys + hhdm);
    klog_puts("[PROC] RIP Dump: ");
    for (int i = 0; i < 128; i++) {
      uint8_t b = code[i];
      const char *hex = "0123456789ABCDEF";
      klog_putchar(hex[(b >> 4) & 0xF]);
      klog_putchar(hex[b & 0xF]);
      klog_putchar(' ');
    }
    klog_puts("\n");
  }

  // DEBUG: Hexdump the stack
  uint64_t rsp_phys = vmm_virt_to_phys((uint64_t *)pml4, user_rsp);
  if (rsp_phys) {
    uint64_t *stack = (uint64_t *)(rsp_phys + hhdm);
    klog_puts("[PROC] RSP Dump: ");
    for (int i = 0; i < 32; i++) {
      klog_uint64(stack[i]);
      klog_putchar(' ');
    }
    klog_puts("\n");
  }

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

static bool do_elf_load(const char *path, uint64_t *pml4, uint64_t load_base, elf_info_t *out_info, char *interp_path, size_t interp_max_len) {
  vfs_node_t *file = vfs_resolve_path(path);
  if (!file) {
    klog_puts("[PROC] ELF load failed: File not found: ");
    klog_puts(path);
    klog_puts("\n");
    return false;
  }

  // Read the ELF Header
  Elf64_Ehdr ehdr;
  if (vfs_read(file, 0, sizeof(Elf64_Ehdr), (uint8_t *)&ehdr) !=
      sizeof(Elf64_Ehdr)) {
    return false;
  }

  if (ehdr.e_ident[0] != 0x7f || ehdr.e_ident[1] != 'E' ||
      ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F' || ehdr.e_ident[4] != 2) {
    return false;
  }

  uint64_t phdr_vaddr = 0;
  uint64_t hhdm = pmm_get_hhdm_offset();

  for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
    Elf64_Phdr phdr;
    uint32_t offset = ehdr.e_phoff + (i * ehdr.e_phentsize);
    if (vfs_read(file, offset, sizeof(Elf64_Phdr), (uint8_t *)&phdr) != sizeof(Elf64_Phdr))
      continue;
    
    if (phdr.p_type == PT_INTERP && interp_path) {
      uint32_t len = phdr.p_filesz;
      if (len >= interp_max_len) len = interp_max_len - 1;
      vfs_read(file, phdr.p_offset, len, (uint8_t *)interp_path);
      interp_path[len] = '\0';
    }

    if (phdr.p_type == PT_LOAD && ehdr.e_phoff >= phdr.p_offset &&
        ehdr.e_phoff < phdr.p_offset + phdr.p_filesz) {
      phdr_vaddr = load_base + phdr.p_vaddr + (ehdr.e_phoff - phdr.p_offset);
    }
  }

  for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
    Elf64_Phdr phdr;
    uint32_t offset = ehdr.e_phoff + (i * ehdr.e_phentsize);

    if (vfs_read(file, offset, sizeof(Elf64_Phdr), (uint8_t *)&phdr) != sizeof(Elf64_Phdr)) continue;
    if (phdr.p_type != PT_LOAD) continue;

    uint64_t vaddr = load_base + phdr.p_vaddr;
    uint32_t filesz = phdr.p_filesz;
    uint32_t memsz = phdr.p_memsz;
    uint32_t file_offset = phdr.p_offset;

    // Track uppermost loaded address for brk base.
    if (load_base == 0) {
      uint64_t seg_end = vaddr + memsz;
      if (seg_end > process_brk_base) {
        process_brk_base = seg_end;
      }
    }

    if (memsz > 0) {
      uint64_t start_page = vaddr & ~(PAGE_SIZE - 1);
      uint64_t end_page = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

      for (uint64_t page = start_page; page < end_page; page += PAGE_SIZE) {
        if (vmm_virt_to_phys(pml4, page) == 0) {
          void *phys = pmm_alloc();
          if (!phys) return false;
          vmm_map_page(pml4, page, (uint64_t)phys, PAGE_FLAG_USER | PAGE_FLAG_RW | PAGE_FLAG_PRESENT);
          uint64_t kernel_virt = (uint64_t)phys + hhdm;
          memset((void *)kernel_virt, 0, PAGE_SIZE);
        }
      }

      if (filesz > 0) {
        vfs_read(file, file_offset, filesz, (uint8_t *)vaddr);
      }
    }
  }

  if (out_info) {
    if (load_base == 0) {
      out_info->entry = ehdr.e_entry;
      out_info->phdr = phdr_vaddr;
      out_info->phentsize = ehdr.e_phentsize;
      out_info->phnum = ehdr.e_phnum;
    } else {
      out_info->interp_base = load_base;
      out_info->interp_entry = load_base + ehdr.e_entry;
    }
  }

  return true;
}

bool elf_load(const char *path, uint64_t *pml4, elf_info_t *out_info) {
  process_brk_base = 0;
  process_brk_current = 0;

  char interp_path[256];
  memset(interp_path, 0, sizeof(interp_path));

  if (!do_elf_load(path, pml4, 0, out_info, interp_path, sizeof(interp_path))) {
    return false;
  }

  if (interp_path[0] != '\0') {
    klog_puts("[PROC] PT_INTERP found: ");
    klog_puts(interp_path);
    klog_puts("\n");
    // Load interpreter at 0x400000000000
    if (!do_elf_load(interp_path, pml4, 0x400000000000ULL, out_info, NULL, 0)) {
      klog_puts("[PROC] Failed to load interpreter\n");
      return false;
    }
  }

  // User stack: [stack_top - stack_size, stack_top); stack_top is unmapped
  // (guard).
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
    // Deep zero the stack page
    uint64_t kernel_virt = (uint64_t)phys + pmm_get_hhdm_offset();
    memset((void *)kernel_virt, 0, PAGE_SIZE);
  }

  // Page-align the brk base upward and set current brk.
  process_brk_base = (process_brk_base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  process_brk_current = process_brk_base;

  // Reset the mmap bump allocator for this new process.
  mm_reset_mmap_state();

  return true;
}

uint64_t process_build_initial_stack(uint64_t stack_top, const char *path,
                                     const char **argv, const char **envp,
                                     const elf_info_t *elf_info) {
  // Create a default environment if none provided
  const char *default_envp[] = {"PATH=/", NULL};
  if (!envp) {
    envp = default_envp;
  }

  if (!argv) {
    const char *temp_argv[2] = {path, NULL};
    argv = temp_argv;
  }

  int argc = 0;
  if (argv)
    while (argv[argc])
      argc++;

  int envc = 0;
  if (envp)
    while (envp[envc])
      envc++;

  // Calculate total string size
  size_t total_size = 0;
  for (int i = 0; i < argc; i++)
    total_size += strlen(argv[i]) + 1;
  for (int i = 0; i < envc; i++)
    total_size += strlen(envp[i]) + 1;

  // Reserve 32 bytes for AT_RANDOM data (16 bytes) and AT_PLATFORM string
  // ("x86_64", 7+1 bytes)
  total_size += 32;

  // Align total_size up to 16 bytes to ensure the string area start is aligned
  total_size = (total_size + 15) & ~15ULL;

  uint64_t string_area = stack_top - total_size;
  string_area &= ~0xFULL;

  // Maintain 16-byte alignment for the stack pointer itself

  size_t argv_ptr_count = (argc > 0) ? (size_t)argc : 1;
  size_t envp_ptr_count = (envc > 0) ? (size_t)envc : 1;
  uint64_t *argv_ptrs = kmalloc(argv_ptr_count * sizeof(uint64_t));
  uint64_t *envp_ptrs = kmalloc(envp_ptr_count * sizeof(uint64_t));
  if (!argv_ptrs || !envp_ptrs) {
    if (argv_ptrs)
      kfree(argv_ptrs);
    if (envp_ptrs)
      kfree(envp_ptrs);
    return 0;
  }

  uint64_t current = string_area;
  for (int i = 0; i < argc; i++) {
    argv_ptrs[i] = current;
    size_t len = strlen(argv[i]) + 1;
    process_copy_to_user(vmm_get_active_pml4(), current, argv[i], len);
    current += len;
  }
  for (int i = 0; i < envc; i++) {
    envp_ptrs[i] = current;
    size_t len = strlen(envp[i]) + 1;
    process_copy_to_user(vmm_get_active_pml4(), current, envp[i], len);
    current += len;
  }

  // Write 16 bytes of pseudo-random data for AT_RANDOM
  uint64_t at_random_addr = current;
  {
    uint8_t rnd[16];
    for (int i = 0; i < 16; i++)
      rnd[i] = (uint8_t)(i * 7 + 0xA5);
    process_copy_to_user(vmm_get_active_pml4(), current, rnd, 16);
    current += 16;
  }

  // Build stack below copied strings to avoid overlap corruption.
  uint64_t stack_ptr = string_area;

  // Layout: argc, argv[argc], NULL, envp[envc], NULL, auxv pairs..., AT_NULL, 0
  // Auxv entries (16 pairs):
  // 1-10: basic ones
  // 11. AT_RANDOM
  // 12. AT_BASE (0)
  // 13. AT_FLAGS (0)
  // 14. AT_HWCAP (0)
  // 15. AT_CLKTCK (100)
  // 16. AT_EXECFN (argv[0])
  // 17. AT_NULL
  // 17 real pairs + 1 NULL pair
  size_t auxv_pairs = elf_info ? 18 : 1;
  size_t stack_entry_count =
      1 + (size_t)argc + 1 + (size_t)envc + 1 + auxv_pairs * 2;
  uint64_t *stack_entries = kmalloc(stack_entry_count * sizeof(uint64_t));
  if (!stack_entries) {
    kfree(argv_ptrs);
    kfree(envp_ptrs);
    return 0;
  }
  int idx = 0;

  stack_entries[idx++] = (uint64_t)argc;
  for (int i = 0; i < argc; i++)
    stack_entries[idx++] = argv_ptrs[i];
  stack_entries[idx++] = 0; // end argv
  for (int i = 0; i < envc; i++)
    stack_entries[idx++] = envp_ptrs[i];
  stack_entries[idx++] = 0; // end envp

  // ── Auxiliary vector ──────────────────────────────────────────────────────
  if (elf_info) {
    // Copy AT_PLATFORM string ("x86_64")
    process_copy_to_user(vmm_get_active_pml4(), at_random_addr + 16, "x86_64",
                         7);

    stack_entries[idx++] = AT_PAGESZ;
    stack_entries[idx++] = PAGE_SIZE;
    stack_entries[idx++] = AT_PHDR;
    stack_entries[idx++] = elf_info->phdr;
    stack_entries[idx++] = AT_PHENT;
    stack_entries[idx++] = elf_info->phentsize;
    stack_entries[idx++] = AT_PHNUM;
    stack_entries[idx++] = elf_info->phnum;
    stack_entries[idx++] = AT_ENTRY;
    stack_entries[idx++] = elf_info->entry;
    stack_entries[idx++] = AT_UID;
    stack_entries[idx++] = 0;
    stack_entries[idx++] = AT_EUID;
    stack_entries[idx++] = 0;
    stack_entries[idx++] = AT_GID;
    stack_entries[idx++] = 0;
    stack_entries[idx++] = AT_EGID;
    stack_entries[idx++] = 0;
    stack_entries[idx++] = AT_SECURE;
    stack_entries[idx++] = 0;
    stack_entries[idx++] = AT_RANDOM;
    stack_entries[idx++] = at_random_addr;
    stack_entries[idx++] = AT_PLATFORM;
    stack_entries[idx++] = at_random_addr + 16; // Use buffer after random data
    stack_entries[idx++] = AT_BASE;
    stack_entries[idx++] = elf_info->interp_base;
    stack_entries[idx++] = AT_FLAGS;
    stack_entries[idx++] = 0;
    stack_entries[idx++] = AT_HWCAP;
    stack_entries[idx++] = 0;
    stack_entries[idx++] = AT_CLKTCK;
    stack_entries[idx++] = 100;
    stack_entries[idx++] = AT_EXECFN;
    stack_entries[idx++] = argv_ptrs[0];
  }
  stack_entries[idx++] = AT_NULL;
  stack_entries[idx++] = 0;

  // Calculate where the final stack pointer will be
  size_t stack_bytes = idx * sizeof(uint64_t);
  uint64_t final_sp = stack_ptr - stack_bytes;
  final_sp &=
      ~0xFULL; // Maintain 16-byte alignment for the stack pointer itself

  klog_puts("[PROC] Final stack layout:\n");
  klog_puts("  argc=");
  klog_uint64(argc);
  klog_puts("  argv[0]=");
  klog_uint64(argv_ptrs[0]);
  klog_puts("  random_addr=");
  klog_uint64(at_random_addr);
  klog_puts("  final_sp=");
  klog_uint64(final_sp);
  klog_puts("  stack_bytes=");
  klog_uint64(stack_bytes);
  klog_puts("\n");

  process_copy_to_user(vmm_get_active_pml4(), final_sp, stack_entries,
                       stack_bytes);

  kfree(argv_ptrs);
  kfree(envp_ptrs);
  kfree(stack_entries);

  return final_sp;
}

bool process_exec_argv(const char **argv) {
  if (!argv || !argv[0])
    return false;

  klog_puts("[PROC] Attempting to execute: ");
  klog_puts(argv[0]);
  klog_puts("\n");

  uint64_t *pml4 = vmm_get_active_pml4();

  elf_info_t elf_info = {0};

  if (!elf_load(argv[0], pml4, &elf_info)) {
    return false;
  }

  klog_puts("[PROC] ELF mapped successfully. Jumping to Ring 3 (RIP: ");
  klog_uint64(elf_info.entry);
  klog_puts(")\n");

  uint64_t user_rsp = process_build_initial_stack(
      ASCENTOS_USER_STACK_TOP, NULL, (const char **)argv, NULL, &elf_info);
  if (!user_rsp) {
    klog_puts("[PROC] Exec failed: could not build initial stack\n");
    return false;
  }

  // Initialize File Descriptors for the main thread
  struct thread *current_thread = sched_get_current();
  if (current_thread) {
    // 0 = stdin, 1 = stdout, 2 = stderr.
    // First try to get the console from the device registry (preferred)
    vfs_node_t *console_node = fb_lookup_device("console");
    // Fall back to VFS lookup if not in registry
    if (!console_node) {
      console_node = vfs_resolve_path("/dev/console");
    }
    current_thread->fds[0] = console_node;
    current_thread->fds[1] = console_node;
    current_thread->fds[2] = console_node;
    current_thread->fd_offsets[0] = 0;
    current_thread->fd_offsets[1] = 0;
    current_thread->fd_offsets[2] = 0;

    for (int i = 3; i < MAX_FDS; i++) {
      current_thread->fds[i] = NULL;
    }
  }

  console_clear();

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
  uint64_t actual_entry = elf_info.interp_base ? elf_info.interp_entry : elf_info.entry;
  process_jump_usermode(actual_entry, user_rsp, (uint64_t)pml4);
  return true;
}
