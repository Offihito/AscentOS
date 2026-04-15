#include "isr.h"
#include "../console/console.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "pic.h"
#include "apic/lapic.h"
#include "msr.h"

const char *exception_messages[] = {"Division By Zero",
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
                                    "Reserved"};

static void print_hex(uint64_t value) {
  const char *hex_chars = "0123456789ABCDEF";
  console_puts("0x");
  for (int i = 15; i >= 0; i--) {
    console_putchar(hex_chars[(value >> (i * 4)) & 0xF]);
  }
}

static void print_gp_error_details(uint64_t err_code) {
  console_puts("GP_ERR_DETAILS: ");
  if (err_code == 0) {
    console_puts("none (not selector-related)\n");
    return;
  }

  console_puts("ext=");
  print_hex(err_code & 0x1);
  console_puts(" tbl=");
  print_hex((err_code >> 1) & 0x3);
  console_puts(" selector_index=");
  print_hex((err_code >> 3) & 0x1FFF);
  console_puts("\n");
}

static void print_reg_line(const char *name, uint64_t value) {
  console_puts(name);
  console_puts(": ");
  print_hex(value);
  console_puts("\n");
}

static void print_pf_error_details(uint64_t err_code) {
  console_puts("PF_ERR_DETAILS: ");
  console_puts("P=");
  print_hex(err_code & 0x1);
  console_puts(" W=");
  print_hex((err_code >> 1) & 0x1);
  console_puts(" U=");
  print_hex((err_code >> 2) & 0x1);
  console_puts(" RSVD=");
  print_hex((err_code >> 3) & 0x1);
  console_puts(" I=");
  print_hex((err_code >> 4) & 0x1);
  console_puts("\n");
}

static void print_yes_no(const char *name, bool value) {
  console_puts(name);
  console_puts("=");
  console_puts(value ? "1 " : "0 ");
}

static bool is_canonical_addr(uint64_t vaddr) {
  uint64_t high = vaddr >> 48;
  return (high == 0x0000ULL) || (high == 0xFFFFULL);
}

static void print_paging_entry_flags(uint64_t entry, bool is_leaf, bool is_pde) {
  print_yes_no("P", (entry & (1ULL << 0)) != 0);
  print_yes_no("RW", (entry & (1ULL << 1)) != 0);
  print_yes_no("US", (entry & (1ULL << 2)) != 0);
  print_yes_no("PWT", (entry & (1ULL << 3)) != 0);
  print_yes_no("PCD", (entry & (1ULL << 4)) != 0);
  print_yes_no("A", (entry & (1ULL << 5)) != 0);
  if (is_leaf || is_pde) {
    print_yes_no("D", (entry & (1ULL << 6)) != 0);
  }
  if (is_pde) {
    print_yes_no("PS", (entry & (1ULL << 7)) != 0);
  } else if (is_leaf) {
    print_yes_no("PAT", (entry & (1ULL << 7)) != 0);
  }
  if (is_leaf) {
    print_yes_no("G", (entry & (1ULL << 8)) != 0);
  }
  print_yes_no("NX", (entry & (1ULL << 63)) != 0);
  console_puts("\n");
}

static void print_entry_summary(const char *name, size_t idx, uint64_t entry,
                                bool is_leaf, bool is_pde) {
  const uint64_t PAGE_MASK = 0x000FFFFFFFFFF000ULL;
  console_puts("  ");
  console_puts(name);
  console_puts("[");
  print_hex(idx);
  console_puts("] raw=");
  print_hex(entry);
  console_puts(" phys_base=");
  print_hex(entry & PAGE_MASK);
  console_puts("\n    flags: ");
  print_paging_entry_flags(entry, is_leaf, is_pde);
}

static void print_neighbor_entries(const char *label, uint64_t *table,
                                   size_t index) {
  size_t start = (index > 1) ? index - 1 : 0;
  size_t end = (index < 510) ? index + 1 : 511;

  console_puts("    ");
  console_puts(label);
  console_puts(" neighborhood:\n");
  for (size_t i = start; i <= end; i++) {
    console_puts("      [");
    print_hex(i);
    console_puts("] = ");
    print_hex(table[i]);
    if (i == index) console_puts("  <target>");
    console_puts("\n");
  }
}

static void print_pf_walk(uint64_t cr2) {
  const uint64_t PAGE_MASK = 0x000FFFFFFFFFF000ULL;
  uint64_t hhdm = pmm_get_hhdm_offset();
  uint64_t *pml4_phys = vmm_get_active_pml4();
  uint64_t *pml4 = (uint64_t *)((uint64_t)pml4_phys + hhdm);
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

  size_t pml4_i = (cr2 >> 39) & 0x1FF;
  size_t pdpt_i = (cr2 >> 30) & 0x1FF;
  size_t pd_i   = (cr2 >> 21) & 0x1FF;
  size_t pt_i   = (cr2 >> 12) & 0x1FF;
  uint64_t page_off = cr2 & 0xFFF;

  console_puts("PF_WALK:\n");
  console_puts("  CR3(raw)      = ");
  print_hex(cr3);
  console_puts("\n");
  console_puts("  CR3(pml4 phys)= ");
  print_hex((uint64_t)pml4_phys);
  console_puts("\n");
  console_puts("  CR3(pml4 virt)= ");
  print_hex((uint64_t)pml4);
  console_puts("\n");
  console_puts("  VA canonical  = ");
  console_puts(is_canonical_addr(cr2) ? "yes\n" : "NO (invalid high bits)\n");
  console_puts("  VA split      = pml4=");
  print_hex(pml4_i);
  console_puts(" pdpt=");
  print_hex(pdpt_i);
  console_puts(" pd=");
  print_hex(pd_i);
  console_puts(" pt=");
  print_hex(pt_i);
  console_puts(" off=");
  print_hex(page_off);
  console_puts("\n");

  uint64_t pml4e = pml4[pml4_i];
  print_entry_summary("PML4E", pml4_i, pml4e, false, false);
  print_neighbor_entries("PML4", pml4, pml4_i);
  if (!(pml4e & 1)) {
    console_puts("  Walk stop: non-present PML4E\n");
    return;
  }

  uint64_t *pdpt = (uint64_t *)((pml4e & PAGE_MASK) + hhdm);
  uint64_t pdpte = pdpt[pdpt_i];
  print_entry_summary("PDPTE", pdpt_i, pdpte, false, false);
  print_neighbor_entries("PDPT", pdpt, pdpt_i);
  if (!(pdpte & 1)) {
    console_puts("  Walk stop: non-present PDPTE\n");
    return;
  }
  if (pdpte & (1ULL << 7)) {
    uint64_t phys_1g = (pdpte & 0x000FFFFFC0000000ULL) | (cr2 & 0x3FFFFFFFULL);
    console_puts("  Walk stop: 1GiB huge page\n");
    console_puts("  Resolved PA   = ");
    print_hex(phys_1g);
    console_puts("\n");
    return;
  }

  uint64_t *pd = (uint64_t *)((pdpte & PAGE_MASK) + hhdm);
  uint64_t pde = pd[pd_i];
  print_entry_summary("PDE", pd_i, pde, false, true);
  print_neighbor_entries("PD", pd, pd_i);
  if (!(pde & 1)) {
    console_puts("  Walk stop: non-present PDE\n");
    return;
  }
  if (pde & (1ULL << 7)) {
    uint64_t phys_2m = (pde & 0x000FFFFFFFE00000ULL) | (cr2 & 0x1FFFFFULL);
    console_puts("  Walk stop: 2MiB huge page\n");
    console_puts("  Resolved PA   = ");
    print_hex(phys_2m);
    console_puts("\n");
    return;
  }

  uint64_t *pt = (uint64_t *)((pde & PAGE_MASK) + hhdm);
  uint64_t pte = pt[pt_i];
  print_entry_summary("PTE", pt_i, pte, true, false);
  print_neighbor_entries("PT", pt, pt_i);
  if (pte & 1ULL) {
    uint64_t final_pa = (pte & PAGE_MASK) | page_off;
    console_puts("  Final PA      = ");
    print_hex(final_pa);
    console_puts("\n");
  } else {
    console_puts("  Walk stop: non-present PTE\n");
  }
  if ((pte & ~PAGE_MASK) != (pte & (PAGE_MASK | (1ULL << 63)))) {
    console_puts("  WARN: PTE has unexpected low/high bits set (possible corruption)\n");
  }
  console_puts("\n");
}

static void print_user_stack_words(uint64_t user_rsp, int words) {
  uint64_t *pml4 = vmm_get_active_pml4();
  console_puts("USER_STACK_TOP:\n");
  for (int i = 0; i < words; i++) {
    uint64_t addr = user_rsp + ((uint64_t)i * sizeof(uint64_t));
    console_puts("  [");
    print_hex(addr);
    console_puts("] = ");
    if (vmm_virt_to_phys(pml4, addr) == 0) {
      console_puts("<unmapped>\n");
      continue;
    }
    print_hex(*(volatile uint64_t *)addr);
    console_puts("\n");
  }
}

// ── Interrupt handler table ──────────────────────────────────────────────────
static isr_t interrupt_handlers[256] = {0};

// ── APIC mode flag ───────────────────────────────────────────────────────────
static bool apic_mode = false;

void isr_set_apic_mode(bool enabled) {
  apic_mode = enabled;
}

void register_interrupt_handler(uint8_t n, isr_t handler) {
  interrupt_handlers[n] = handler;
}

// ── EOI dispatch ─────────────────────────────────────────────────────────────
// Routes the End-of-Interrupt signal to the correct controller depending on
// whether we are in legacy PIC mode or APIC mode.
static void send_eoi(struct registers *regs) {
  // Never send EOI for LAPIC spurious interrupts (vector 0xFF)
  if (regs->int_no == LAPIC_SPURIOUS_VECTOR) return;

  // CPU exceptions (vectors 0-31) are not delivered by any interrupt
  // controller – no EOI required.
  if (regs->int_no < 32) return;

  if (apic_mode) {
    lapic_send_eoi();
  } else {
    if (regs->int_no <= 47) {
      pic_send_eoi(regs->int_no - 32);
    }
  }
}

// ── Common ISR handler (called from assembly stub) ───────────────────────────
void isr_handler(struct registers *regs) {
  // If we have a custom handler attached, route to it
  if (interrupt_handlers[regs->int_no] != 0) {
    isr_t handler = interrupt_handlers[regs->int_no];
    handler(regs);
    send_eoi(regs);
    return;
  }

  // Ignore unhandled hardware interrupts (just send EOI)
  if (regs->int_no >= 32) {
    send_eoi(regs);
    return;
  }

  // ── CPU Exception → KERNEL PANIC ──────────────────────────────────────
  console_clear();
  console_puts("==================== KERNEL PANIC ====================\n");
  if (regs->int_no < 32) {
    console_puts(exception_messages[regs->int_no]);
    console_puts(" Exception\n");
  } else {
    console_puts("Unknown Exception\n");
  }

  console_puts("INT_NO: ");
  print_hex(regs->int_no);
  console_puts("\n");
  console_puts("ERR_CD: ");
  print_hex(regs->err_code);
  console_puts("\n\n");

  console_puts("RIP:    ");
  print_hex(regs->rip);
  console_puts("\n");
  console_puts("RSP:    ");
  print_hex(regs->rsp);
  console_puts("\n");
  console_puts("RFLAGS: ");
  print_hex(regs->rflags);
  console_puts("\n");
  console_puts("CS:     ");
  print_hex(regs->cs);
  console_puts("\n");
  console_puts("SS:     ");
  print_hex(regs->ss);
  console_puts("\n");

  print_reg_line("RAX", regs->rax);
  print_reg_line("RBX", regs->rbx);
  print_reg_line("RCX", regs->rcx);
  print_reg_line("RDX", regs->rdx);
  print_reg_line("RSI", regs->rsi);
  print_reg_line("RDI", regs->rdi);
  print_reg_line("RBP", regs->rbp);
  print_reg_line("R8", regs->r8);
  print_reg_line("R9", regs->r9);
  print_reg_line("R10", regs->r10);
  print_reg_line("R11", regs->r11);
  print_reg_line("R12", regs->r12);
  print_reg_line("R13", regs->r13);
  print_reg_line("R14", regs->r14);
  print_reg_line("R15", regs->r15);

  if (regs->int_no == 13) {
    print_gp_error_details(regs->err_code);
    if ((regs->cs & 0x3) == 0x3) {
      print_user_stack_words(regs->rsp, 4);
    }
  }

  console_puts("\nTLS/MSR STATE:\n");
  print_reg_line("IA32_FS_BASE", rdmsr(0xC0000100));
  print_reg_line("IA32_GS_BASE", rdmsr(0xC0000101));
  print_reg_line("IA32_KERNEL_GS_BASE", rdmsr(0xC0000102));

  if (regs->int_no == 14) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    console_puts("\n=> PAGE FAULT ADDRESS (CR2): ");
    print_hex(cr2);
    console_puts("\n");
    print_pf_error_details(regs->err_code);
    print_pf_walk(cr2);
  }

  console_puts("\nSystem Halted.\n");

  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}
