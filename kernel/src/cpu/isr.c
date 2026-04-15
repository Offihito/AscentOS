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

// ── Low-level output helpers ──────────────────────────────────────────────────

static void print_hex(uint64_t value) {
  const char *hex_chars = "0123456789ABCDEF";
  console_puts("0x");
  for (int i = 15; i >= 0; i--) {
    console_putchar(hex_chars[(value >> (i * 4)) & 0xF]);
  }
}

static void print_hex8(uint8_t value) {
  const char *hex_chars = "0123456789ABCDEF";
  console_puts("0x");
  console_putchar(hex_chars[(value >> 4) & 0xF]);
  console_putchar(hex_chars[value & 0xF]);
}

static void print_dec(uint64_t value) {
  if (value == 0) { console_putchar('0'); return; }
  char buf[21];
  int i = 0;
  while (value > 0) { buf[i++] = '0' + (value % 10); value /= 10; }
  for (int j = i - 1; j >= 0; j--) console_putchar(buf[j]);
}

static void print_reg_line(const char *name, uint64_t value) {
  console_puts(name);
  console_puts(": ");
  print_hex(value);
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

// ── RFLAGS decoder ────────────────────────────────────────────────────────────

static void print_rflags_decoded(uint64_t rflags) {
  console_puts("RFLAGS: ");
  print_hex(rflags);
  console_puts("\n  Flags: ");
  print_yes_no("CF",  (rflags >> 0)  & 1);
  print_yes_no("PF",  (rflags >> 2)  & 1);
  print_yes_no("AF",  (rflags >> 4)  & 1);
  print_yes_no("ZF",  (rflags >> 6)  & 1);
  print_yes_no("SF",  (rflags >> 7)  & 1);
  print_yes_no("TF",  (rflags >> 8)  & 1);
  print_yes_no("IF",  (rflags >> 9)  & 1);
  print_yes_no("DF",  (rflags >> 10) & 1);
  print_yes_no("OF",  (rflags >> 11) & 1);
  console_puts("\n  IOPL=");
  print_dec((rflags >> 12) & 0x3);
  print_yes_no(" NT",  (rflags >> 14) & 1);
  print_yes_no("RF",  (rflags >> 16) & 1);
  print_yes_no("VM",  (rflags >> 17) & 1);
  print_yes_no("AC",  (rflags >> 18) & 1);
  print_yes_no("VIF", (rflags >> 19) & 1);
  print_yes_no("VIP", (rflags >> 20) & 1);
  print_yes_no("ID",  (rflags >> 21) & 1);
  console_puts("\n");
}

// ── Control register diagnostics ─────────────────────────────────────────────

static void print_cr_state(void) {
  uint64_t cr0, cr3, cr4;
  __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

  console_puts("\nCONTROL REGISTERS:\n");

  console_puts("CR0: ");
  print_hex(cr0);
  console_puts("\n  ");
  print_yes_no("PE",  (cr0 >> 0)  & 1);
  print_yes_no("WP",  (cr0 >> 16) & 1);
  print_yes_no("PG",  (cr0 >> 31) & 1);
  console_puts("\n");

  console_puts("CR3: ");
  print_hex(cr3);
  console_puts("\n  PCID=");
  print_hex(cr3 & 0xFFF);
  console_puts(" PML4_PHYS=");
  print_hex(cr3 & ~0xFFFULL);
  console_puts("\n");

  console_puts("CR4: ");
  print_hex(cr4);
  console_puts("\n  ");
  print_yes_no("PSE",    (cr4 >> 4)  & 1);
  print_yes_no("PAE",    (cr4 >> 5)  & 1);
  print_yes_no("PGE",    (cr4 >> 7)  & 1);
  print_yes_no("SMEP",   (cr4 >> 20) & 1);
  print_yes_no("SMAP",   (cr4 >> 21) & 1);
  print_yes_no("PKE",    (cr4 >> 22) & 1);
  print_yes_no("CET",    (cr4 >> 23) & 1);
  console_puts("\n");
}

// ── GP fault decoder ──────────────────────────────────────────────────────────

static void print_gp_error_details(uint64_t err_code) {
  console_puts("GP_ERR_DETAILS: ");
  if (err_code == 0) {
    console_puts("none (not selector-related)\n");
    return;
  }
  const char *tbl_names[] = {"GDT", "IDT", "LDT", "IDT"};
  uint8_t ext  = err_code & 0x1;
  uint8_t tbl  = (err_code >> 1) & 0x3;
  uint16_t idx = (err_code >> 3) & 0x1FFF;
  console_puts("ext=");
  print_hex8(ext);
  console_puts(" tbl=");
  console_puts(tbl_names[tbl]);
  console_puts(" idx=");
  print_dec(idx);
  console_puts(" (");
  print_hex(err_code);
  console_puts(")\n");
}

// ── PF fault error code decoder ───────────────────────────────────────────────

static void print_pf_error_details(uint64_t err_code) {
  bool p    = (err_code >> 0) & 1;  /* page present */
  bool w    = (err_code >> 1) & 1;  /* write */
  bool u    = (err_code >> 2) & 1;  /* user mode */
  bool rsvd = (err_code >> 3) & 1;  /* reserved bit set in PTE */
  bool i    = (err_code >> 4) & 1;  /* instruction fetch */
  bool pk   = (err_code >> 5) & 1;  /* protection key */
  bool ss   = (err_code >> 6) & 1;  /* shadow stack */

  console_puts("PF_ERR_DETAILS: ");
  print_yes_no("P", p);
  print_yes_no("W", w);
  print_yes_no("U", u);
  print_yes_no("RSVD", rsvd);
  print_yes_no("I", i);
  print_yes_no("PK", pk);
  print_yes_no("SS", ss);
  console_puts("\n  Cause: ");
  if (i)         console_puts("instruction fetch from ");
  else if (w)    console_puts("write to ");
  else           console_puts("read from ");
  console_puts(p ? "present page (protection violation)" : "non-present page");
  if (u)         console_puts(", from user mode");
  else           console_puts(", from kernel mode");
  if (rsvd)      console_puts(" [RSVD BIT IN PTE - possible corruption]");
  if (pk)        console_puts(" [protection-key violation]");
  if (ss)        console_puts(" [shadow stack violation]");
  console_puts("\n");
}

// ── PTE corruption analysis ───────────────────────────────────────────────────
//
// A well-formed PTE has its physical address bits in [51:12].  Any bits set in
// the "software available" range [62:52] or outside the implemented PA width
// (assuming 52-bit max per the AMD64 spec) indicate corruption.  We also check
// for conflicting flag combinations.

static void analyze_pte_corruption(uint64_t pte) {
  const uint64_t PA_MASK    = 0x000FFFFFFFFFF000ULL;
  const uint64_t LOW_MASK   = 0xFFFULL;              /* bits [11:0] */
  const uint64_t HIGH_MASK  = 0x7FF0000000000000ULL; /* bits [62:52] */

  uint64_t low_bits  = pte & LOW_MASK;
  uint64_t high_bits = pte & HIGH_MASK;
  uint64_t pa        = pte & PA_MASK;

  bool present    = (pte >> 0)  & 1;
  bool rw         = (pte >> 1)  & 1;
  bool us         = (pte >> 2)  & 1;
  bool dirty      = (pte >> 6)  & 1;
  bool global     = (pte >> 8)  & 1;
  bool nx         = (pte >> 63) & 1;

  console_puts("  PTE_CORRUPTION_ANALYSIS:\n");
  console_puts("    raw=");
  print_hex(pte);
  console_puts("\n    PA field=");
  print_hex(pa);
  console_puts("\n    low_bits[11:0]=");
  print_hex(low_bits);
  console_puts(" high_bits[62:52]=");
  print_hex(high_bits);
  console_puts("\n");

  if (high_bits != 0) {
    console_puts("    WARN: bits [62:52] are non-zero (");
    print_hex(high_bits);
    console_puts(") -- reserved or OS-specific bits set; likely corruption\n");
  }

  /* Detect packed-word pattern: upper and lower 32-bit halves are suspiciously
   * similar (common symptom of a struct written to the wrong field or a
   * 32-bit store to a 64-bit PTE slot). */
  uint32_t lo32 = (uint32_t)(pte & 0xFFFFFFFFULL);
  uint32_t hi32 = (uint32_t)(pte >> 32);
  if (hi32 != 0 && hi32 != 0xFFFFFFFF) {
    uint32_t diff = lo32 ^ hi32;
    if (__builtin_popcount(diff) <= 4) {
      console_puts("    WARN: upper and lower 32-bit halves differ by only ");
      print_dec(__builtin_popcount(diff));
      console_puts(" bit(s) (lo=");
      print_hex(lo32);
      console_puts(" hi=");
      print_hex(hi32);
      console_puts(") -- looks like a 32-bit value replicated into both halves\n");
    }
  }

  /* Flag-combination sanity checks */
  if (!present && dirty)  console_puts("    WARN: D=1 but P=0 (dirty non-present page)\n");
  if (!present && global) console_puts("    WARN: G=1 but P=0 (global non-present page)\n");
  if (!rw && dirty)       console_puts("    NOTE: D=1 but RW=0 (was writable, now read-only)\n");
  if (nx && !us && !present)
    console_puts("    NOTE: NX + kernel + non-present\n");

  if (high_bits == 0 && lo32 == 0 && hi32 == 0)
    console_puts("    PTE is completely zero (was never mapped or was explicitly cleared)\n");
}

// ── Paging-entry flag printer ─────────────────────────────────────────────────

static void print_paging_entry_flags(uint64_t entry, bool is_leaf, bool is_pde) {
  print_yes_no("P",   (entry & (1ULL << 0))  != 0);
  print_yes_no("RW",  (entry & (1ULL << 1))  != 0);
  print_yes_no("US",  (entry & (1ULL << 2))  != 0);
  print_yes_no("PWT", (entry & (1ULL << 3))  != 0);
  print_yes_no("PCD", (entry & (1ULL << 4))  != 0);
  print_yes_no("A",   (entry & (1ULL << 5))  != 0);
  if (is_leaf || is_pde)
    print_yes_no("D",   (entry & (1ULL << 6))  != 0);
  if (is_pde)
    print_yes_no("PS",  (entry & (1ULL << 7))  != 0);
  else if (is_leaf)
    print_yes_no("PAT", (entry & (1ULL << 7))  != 0);
  if (is_leaf)
    print_yes_no("G",   (entry & (1ULL << 8))  != 0);
  print_yes_no("NX",  (entry & (1ULL << 63)) != 0);
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
  size_t end   = (index < 510) ? index + 1 : 511;

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

// ── Full page-table walk with enriched PTE analysis ──────────────────────────

static void print_pf_walk(uint64_t cr2) {
  const uint64_t PAGE_MASK = 0x000FFFFFFFFFF000ULL;
  uint64_t hhdm      = pmm_get_hhdm_offset();
  uint64_t *pml4_phys = vmm_get_active_pml4();
  uint64_t *pml4      = (uint64_t *)((uint64_t)pml4_phys + hhdm);
  uint64_t cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

  size_t   pml4_i  = (cr2 >> 39) & 0x1FF;
  size_t   pdpt_i  = (cr2 >> 30) & 0x1FF;
  size_t   pd_i    = (cr2 >> 21) & 0x1FF;
  size_t   pt_i    = (cr2 >> 12) & 0x1FF;
  uint64_t page_off = cr2 & 0xFFF;

  console_puts("PF_WALK:\n");
  console_puts("  CR3(raw)      = "); print_hex(cr3);       console_puts("\n");
  console_puts("  CR3(pml4 phys)= "); print_hex((uint64_t)pml4_phys); console_puts("\n");
  console_puts("  CR3(pml4 virt)= "); print_hex((uint64_t)pml4);      console_puts("\n");
  console_puts("  VA canonical  = ");
  console_puts(is_canonical_addr(cr2) ? "yes\n" : "NO (invalid high bits)\n");
  console_puts("  VA split: pml4=");
  print_hex(pml4_i);
  console_puts(" pdpt="); print_hex(pdpt_i);
  console_puts(" pd=");   print_hex(pd_i);
  console_puts(" pt=");   print_hex(pt_i);
  console_puts(" off=");  print_hex(page_off);
  console_puts("\n");

  uint64_t pml4e = pml4[pml4_i];
  print_entry_summary("PML4E", pml4_i, pml4e, false, false);
  print_neighbor_entries("PML4", pml4, pml4_i);
  if (!(pml4e & 1)) {
    console_puts("  Walk stop: non-present PML4E\n");
    return;
  }

  uint64_t *pdpt  = (uint64_t *)((pml4e & PAGE_MASK) + hhdm);
  uint64_t  pdpte = pdpt[pdpt_i];
  print_entry_summary("PDPTE", pdpt_i, pdpte, false, false);
  print_neighbor_entries("PDPT", pdpt, pdpt_i);
  if (!(pdpte & 1)) {
    console_puts("  Walk stop: non-present PDPTE\n");
    return;
  }
  if (pdpte & (1ULL << 7)) {
    uint64_t phys_1g = (pdpte & 0x000FFFFFC0000000ULL) | (cr2 & 0x3FFFFFFFULL);
    console_puts("  Walk stop: 1GiB huge page\n  Resolved PA = ");
    print_hex(phys_1g);
    console_puts("\n");
    return;
  }

  uint64_t *pd  = (uint64_t *)((pdpte & PAGE_MASK) + hhdm);
  uint64_t  pde = pd[pd_i];
  print_entry_summary("PDE", pd_i, pde, false, true);
  print_neighbor_entries("PD", pd, pd_i);
  if (!(pde & 1)) {
    console_puts("  Walk stop: non-present PDE\n");
    return;
  }
  if (pde & (1ULL << 7)) {
    uint64_t phys_2m = (pde & 0x000FFFFFFFE00000ULL) | (cr2 & 0x1FFFFFULL);
    console_puts("  Walk stop: 2MiB huge page\n  Resolved PA = ");
    print_hex(phys_2m);
    console_puts("\n");
    return;
  }

  uint64_t *pt  = (uint64_t *)((pde & PAGE_MASK) + hhdm);
  uint64_t  pte = pt[pt_i];
  print_entry_summary("PTE", pt_i, pte, true, false);
  print_neighbor_entries("PT", pt, pt_i);

  if (pte & 1ULL) {
    uint64_t final_pa = (pte & PAGE_MASK) | page_off;
    console_puts("  Final PA = ");
    print_hex(final_pa);
    console_puts("\n");
  } else {
    console_puts("  Walk stop: non-present PTE\n");
    /* Always run deep analysis on a non-present PTE -- the raw value may
     * reveal what kind of error caused it (corruption, swap, COW, ...) */
    analyze_pte_corruption(pte);
  }

  /* Run corruption check even for present PTEs with suspicious bits. */
  const uint64_t RESERVED_MASK = 0x7FF0000000000000ULL;
  if ((pte & 1ULL) && (pte & RESERVED_MASK)) {
    console_puts("  WARN: present PTE has reserved bits set!\n");
    analyze_pte_corruption(pte);
  }
  console_puts("\n");
}

// ── User-stack word dump ──────────────────────────────────────────────────────

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

// ── Privilege / context summary ───────────────────────────────────────────────

static void print_context_summary(struct registers *regs) {
  uint8_t cpl = regs->cs & 0x3;
  console_puts("CONTEXT: ");
  if      (cpl == 0) console_puts("kernel (ring 0)");
  else if (cpl == 1) console_puts("ring 1 (unusual)");
  else if (cpl == 2) console_puts("ring 2 (unusual)");
  else               console_puts("user   (ring 3)");
  console_puts("  CS=");
  print_hex(regs->cs);
  console_puts("  SS=");
  print_hex(regs->ss);
  console_puts("\n");
}

// ── Interrupt handler table ───────────────────────────────────────────────────
static isr_t interrupt_handlers[256] = {0};

// ── APIC mode flag ────────────────────────────────────────────────────────────
static bool apic_mode = false;

void isr_set_apic_mode(bool enabled) {
  apic_mode = enabled;
}

void register_interrupt_handler(uint8_t n, isr_t handler) {
  interrupt_handlers[n] = handler;
}

// ── EOI dispatch ──────────────────────────────────────────────────────────────
static void send_eoi(struct registers *regs) {
  if (regs->int_no == LAPIC_SPURIOUS_VECTOR) return;
  if (regs->int_no < 32) return;

  if (apic_mode) {
    lapic_send_eoi();
  } else {
    if (regs->int_no <= 47)
      pic_send_eoi(regs->int_no - 32);
  }
}

// ── Common ISR handler (called from assembly stub) ────────────────────────────
void isr_handler(struct registers *regs) {
  if (interrupt_handlers[regs->int_no] != 0) {
    isr_t handler = interrupt_handlers[regs->int_no];
    handler(regs);
    send_eoi(regs);
    return;
  }

  if (regs->int_no >= 32) {
    send_eoi(regs);
    return;
  }

  // ── CPU Exception → KERNEL PANIC ─────────────────────────────────────────
  console_clear();
  console_puts("==================== KERNEL PANIC ====================\n");
  if (regs->int_no < 32) {
    console_puts(exception_messages[regs->int_no]);
    console_puts(" Exception");
  } else {
    console_puts("Unknown Exception");
  }
  console_puts("  [INT ");
  print_dec(regs->int_no);
  console_puts("]\n");

  console_puts("ERR_CODE: ");
  print_hex(regs->err_code);
  console_puts("\n\n");

  // Context / privilege level
  print_context_summary(regs);
  console_puts("\n");

  // RIP / RSP first – most useful for locating the fault
  console_puts("RIP:    "); print_hex(regs->rip); console_puts("\n");
  console_puts("RSP:    "); print_hex(regs->rsp); console_puts("\n");
  print_rflags_decoded(regs->rflags);
  console_puts("\n");

  // GPRs
  print_reg_line("RAX", regs->rax);
  print_reg_line("RBX", regs->rbx);
  print_reg_line("RCX", regs->rcx);
  print_reg_line("RDX", regs->rdx);
  print_reg_line("RSI", regs->rsi);
  print_reg_line("RDI", regs->rdi);
  print_reg_line("RBP", regs->rbp);
  print_reg_line("R8",  regs->r8);
  print_reg_line("R9",  regs->r9);
  print_reg_line("R10", regs->r10);
  print_reg_line("R11", regs->r11);
  print_reg_line("R12", regs->r12);
  print_reg_line("R13", regs->r13);
  print_reg_line("R14", regs->r14);
  print_reg_line("R15", regs->r15);

  // Exception-specific sections
  if (regs->int_no == 13) {
    console_puts("\n");
    print_gp_error_details(regs->err_code);
    if ((regs->cs & 0x3) == 0x3)
      print_user_stack_words(regs->rsp, 8);
  }

  if (regs->int_no == 14) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    console_puts("\n=> PAGE FAULT ADDRESS (CR2): ");
    print_hex(cr2);
    console_puts("\n");

    // Call out instruction-fetch faults explicitly (CR2 == RIP means the CPU
    // could not fetch the next instruction -- very different root cause from a
    // data access fault at the same address).
    if (cr2 == regs->rip) {
      console_puts("  NOTE: CR2 == RIP -- this is an instruction fetch fault,\n");
      console_puts("        not a data access. The page containing the next\n");
      console_puts("        instruction to execute is not mapped.\n");
    }

    print_pf_error_details(regs->err_code);
    print_pf_walk(cr2);
  }

  // Control register state (CR0 / CR3 / CR4 flags)
  print_cr_state();

  // MSR state
  console_puts("\nMSR STATE:\n");
  print_reg_line("IA32_FS_BASE",        rdmsr(0xC0000100));
  print_reg_line("IA32_GS_BASE",        rdmsr(0xC0000101));
  print_reg_line("IA32_KERNEL_GS_BASE", rdmsr(0xC0000102));
  print_reg_line("EFER",                rdmsr(0xC0000080));

  console_puts("\nSystem Halted.\n");
  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}