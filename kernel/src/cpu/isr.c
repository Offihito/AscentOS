#include "isr.h"
#include "../console/console.h"
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
  }

  console_puts("\nSystem Halted.\n");

  for (;;) {
    __asm__ volatile("cli; hlt");
  }
}
