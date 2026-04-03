#include "console/console.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/isr.h"
#include "acpi/acpi.h"
#include "apic/lapic.h"
#include "apic/ioapic.h"
#include "smp/cpu.h"
#include "cpu/pic.h"
#include "drivers/keyboard.h"
#include "drivers/pit.h"
#include "io/io.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "shell/shell.h"
#include <limine.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

__attribute__((used,
               section(".limine_requests_start"))) static volatile uint64_t
    limine_requests_start_marker[4] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests"))) static volatile uint64_t
    limine_base_revision[3] = LIMINE_BASE_REVISION(0);

__attribute__((
    used,
    section(
        ".limine_requests"))) static volatile struct limine_framebuffer_request
    framebuffer_request = {.id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0};

__attribute__((
    used,
    section(
        ".limine_requests"))) static volatile struct limine_paging_mode_request
    paging_mode_request = {.id = LIMINE_PAGING_MODE_REQUEST_ID,
                           .revision = 0,
                           .mode = LIMINE_PAGING_MODE_X86_64_4LVL,
                           .max_mode = 0,
                           .min_mode = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_memmap_request
    memmap_request = {.id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_hhdm_request
    hhdm_request = {.id = LIMINE_HHDM_REQUEST_ID, .revision = 0};

__attribute__((
    used,
    section(".limine_requests"))) static volatile struct limine_rsdp_request
    rsdp_request = {.id = LIMINE_RSDP_REQUEST_ID, .revision = 0};

__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t
    limine_requests_end_marker[2] = LIMINE_REQUESTS_END_MARKER;

static void halt(void) {
  for (;;) {
    __asm__ volatile("hlt");
  }
}

static void print_uint64(uint64_t num) {
  if (num == 0) {
    console_puts("0");
    return;
  }
  char buf[20];
  int i = 0;
  while (num > 0) {
    buf[i++] = '0' + (num % 10);
    num /= 10;
  }
  while (i > 0) {
    i--;
    char str[2] = {buf[i], '\0'};
    console_puts(str);
  }
}

void kmain(void) {
  if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
    halt();
  }

  if (framebuffer_request.response == NULL ||
      framebuffer_request.response->framebuffer_count < 1) {
    halt();
  }

  struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
  console_init(fb);

  if (paging_mode_request.response != NULL) {
    if (paging_mode_request.response->mode == LIMINE_PAGING_MODE_X86_64_4LVL) {
      console_puts("[OK] Limine Paging Mode: 4-level (x86_64)\n");
    } else if (paging_mode_request.response->mode ==
               LIMINE_PAGING_MODE_X86_64_5LVL) {
      console_puts("[OK] Limine Paging Mode: 5-level (x86_64)\n");
    } else {
      console_puts("[OK] Limine Paging Mode: Unknown\n");
    }
  } else {
    console_puts("[WARN] Paging mode response not provided by Limine.\n");
  }

  // ═══════════════════════════════════════════════════════════════════════
  //  Phase 1: CPU descriptor tables
  // ═══════════════════════════════════════════════════════════════════════
  gdt_init();
  idt_init();

  // ═══════════════════════════════════════════════════════════════════════
  //  Phase 2: Legacy PIC — used temporarily until APIC takes over
  // ═══════════════════════════════════════════════════════════════════════
  pic_remap(32, 40);
  outb(0x21, 0xFC); // unmask IRQ0 (PIT) and IRQ1 (Keyboard)
  outb(0xA1, 0xFF); // mask everything on slave PIC

  pit_init(100);
  console_puts("[OK] Legacy PIC Remapped and PIT 100Hz started.\n");

  keyboard_init();
  __asm__ volatile("sti"); // Enable hardware interrupts!

  // ═══════════════════════════════════════════════════════════════════════
  //  Phase 3: Memory management
  // ═══════════════════════════════════════════════════════════════════════
  if (memmap_request.response == NULL || hhdm_request.response == NULL) {
    console_puts(
        "[ERR] Missing Limine memory map or HHDM responses. Halting.\n");
    halt();
  }

  pmm_init(memmap_request.response, hhdm_request.response->offset);

  console_puts("[OK] Physical Memory Manager (PMM) Initialized.\n");
  console_puts("     Total RAM:  ");
  print_uint64(pmm_get_total_memory() / (1024 * 1024));
  console_puts(" MB\n");

  console_puts("     Usable RAM: ");
  print_uint64(pmm_get_usable_memory() / (1024 * 1024));
  console_puts(" MB\n\n");

  console_puts("[OK] Reclaiming Limine Bootloader Memory...\n");
  pmm_reclaim_bootloader();
  console_puts("     Optimized Usable RAM: ");
  print_uint64(pmm_get_usable_memory() / (1024 * 1024));
  console_puts(" MB\n\n");

  console_puts("[OK] Initializing Virtual Memory Manager (VMM)...\n");
  vmm_init();
  console_puts("     Active CR3 Page Map hooked.\n");

  console_puts("[OK] Testing VMM mapping... (0xCAFEBABE000)\n");
  void *test_phys = pmm_alloc();
  if (test_phys) {
    uint64_t vaddr = 0xCAFEBABE000;
    vmm_map_page(vmm_get_active_pml4(), vaddr, (uint64_t)test_phys,
                 PAGE_FLAG_RW | PAGE_FLAG_USER);

    volatile uint64_t *test_ptr = (volatile uint64_t *)vaddr;
    *test_ptr = 0x1337BEEF; // If this page faults, the mapping failed!

    if (*test_ptr == 0x1337BEEF) {
      console_puts("     VMM custom mapping test SUCCESSFUL!\n");
    } else {
      console_puts("     VMM custom mapping test FAILED!\n");
    }
  }

  // ═══════════════════════════════════════════════════════════════════════
  //  Phase 4: ACPI discovery
  // ═══════════════════════════════════════════════════════════════════════
  acpi_init(rsdp_request.response);

  // ═══════════════════════════════════════════════════════════════════════
  //  Phase 4.5: (Moved to end of Phase 5)
  // ═══════════════════════════════════════════════════════════════════════

  // ═══════════════════════════════════════════════════════════════════════
  //  Phase 5: Transition from legacy PIC → APIC
  // ═══════════════════════════════════════════════════════════════════════
  uint32_t lapic_base = acpi_get_lapic_base();
  uint32_t ioapic_base = acpi_get_ioapic_base();

  if (lapic_base && ioapic_base) {
    console_puts("\n[INFO] Switching to APIC interrupt mode...\n");

    // Disable interrupts during the transition
    __asm__ volatile("cli");

    // ── 5a. Disable the legacy 8259 PIC ─────────────────────────────────
    pic_disable();
    console_puts("[OK] Legacy 8259 PIC disabled.\n");

    // ── 5b. Initialize the Local APIC ───────────────────────────────────
    lapic_init((uint64_t)lapic_base);

    // ── 5c. Initialize the I/O APIC ─────────────────────────────────────
    ioapic_init((uint64_t)ioapic_base, acpi_get_ioapic_gsi_base());

    // ── 5d. Route PIT (IRQ 0) through the I/O APIC ─────────────────────
    uint32_t pit_gsi = 0;
    uint16_t pit_flags = 0;
    acpi_get_irq_override(0, &pit_gsi, &pit_flags);
    ioapic_route_irq((uint8_t)pit_gsi, 32, (uint8_t)lapic_get_id(), pit_flags);
    console_puts("[OK] PIT routed: GSI ");
    {
      char c = '0' + (char)pit_gsi;
      console_putchar(c);
    }
    console_puts(" -> Vector 32\n");

    // ── 5e. Route Keyboard (IRQ 1) through the I/O APIC ────────────────
    uint32_t kbd_gsi = 1;
    uint16_t kbd_flags = 0;
    acpi_get_irq_override(1, &kbd_gsi, &kbd_flags);
    ioapic_route_irq((uint8_t)kbd_gsi, 33, (uint8_t)lapic_get_id(), kbd_flags);
    console_puts("[OK] Keyboard routed: GSI ");
    {
      char c = '0' + (char)kbd_gsi;
      console_putchar(c);
    }
    console_puts(" -> Vector 33\n");

    // ── 5f. Switch ISR EOI routing to LAPIC ─────────────────────────────
    isr_set_apic_mode(true);

    // Re-enable interrupts — now handled through the APIC path
    __asm__ volatile("sti");

    console_puts("[OK] APIC interrupt mode ACTIVE.\n\n");
  } else {
    console_puts(
        "[WARN] APIC hardware not detected — staying with legacy PIC.\n\n");
  }

  // ═══════════════════════════════════════════════════════════════════════
  //  Phase 5.5: SMP initialization (wake up APs)
  // ═══════════════════════════════════════════════════════════════════════
  cpu_init();

  // ═══════════════════════════════════════════════════════════════════════
  //  Phase 6: Kernel heap
  // ═══════════════════════════════════════════════════════════════════════
  console_puts("[OK] Initializing Kernel Heap...\n");
  heap_init();
  char *heap_test = kmalloc(64);
  if (heap_test) {
    const char *test_msg = "     Heap allocation SUCCESSFUL!\n";
    int i = 0;
    while (test_msg[i] != '\0') {
      heap_test[i] = test_msg[i];
      i++;
    }
    heap_test[i] = '\0';
    console_puts(heap_test);
    kfree(heap_test);
  } else {
    console_puts("     Heap allocation FAILED!\n");
  }

  // ═══════════════════════════════════════════════════════════════════════
  //  Phase 7: Interactive shell
  // ═══════════════════════════════════════════════════════════════════════
  console_puts("\nKernel initialization complete. Starting shell...\n");
  shell_init();
  shell_run();
}
