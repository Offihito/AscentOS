#include "acpi/acpi.h"
#include "apic/ioapic.h"
#include "apic/lapic.h"
#include "apic/lapic_timer.h"
#include "console/console.h"
#include "console/klog.h"
#include "cpu/features.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "cpu/isr.h"
#include "cpu/pic.h"
#include "drivers/input/keyboard.h"
#include "drivers/net/rtl8139.h"
#include "drivers/pci/pci.h"
#include "drivers/serial.h"
#include "drivers/storage/ahci.h"
#include "drivers/storage/block.h"
#include "drivers/timer/pit.h"
#include "fb/framebuffer.h"
#include "fs/ext2.h"
#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "io/io.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "net/net.h"
#include "sched/sched.h"
#include "shell/shell.h"
#include "smp/cpu.h"
#include "syscalls/syscall.h"
#include <limine.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

void kmain(void) {
  if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
    halt();
  }

  if (framebuffer_request.response == NULL ||
      framebuffer_request.response->framebuffer_count < 1) {
    halt();
  }

  struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
  serial_init();
  console_init(fb);

  if (paging_mode_request.response != NULL) {
    if (paging_mode_request.response->mode == LIMINE_PAGING_MODE_X86_64_4LVL) {
      klog_puts("[OK] Limine Paging Mode: 4-level (x86_64)\n");
    } else if (paging_mode_request.response->mode ==
               LIMINE_PAGING_MODE_X86_64_5LVL) {
      klog_puts("[OK] Limine Paging Mode: 5-level (x86_64)\n");
    } else {
      klog_puts("[OK] Limine Paging Mode: Unknown\n");
    }
  } else {
    klog_puts("[WARN] Paging mode response not provided by Limine.\n");
  }

  // ═══════════════════════════════════════════════════════════════════════
  //  Phase 1: CPU descriptor tables
  // ═══════════════════════════════════════════════════════════════════════
  gdt_init();
  cpu_features_init();
  idt_init();
  syscall_init();

  // ═══════════════════════════════════════════════════════════════════════
  //  Phase 2: Legacy PIC — used temporarily until APIC takes over
  // ═══════════════════════════════════════════════════════════════════════
  pic_remap(32, 40);
  outb(0x21, 0xFC); // unmask IRQ0 (PIT) and IRQ1 (Keyboard)
  outb(0xA1, 0xFF); // mask everything on slave PIC

  pit_init(100);
  klog_puts("[OK] Legacy PIC Remapped and PIT 100Hz started.\n");

  keyboard_init();
  __asm__ volatile("sti"); // Enable hardware interrupts!

  // ═══════════════════════════════════════════════════════════════════════
  //  Phase 3: Memory management
  // ═══════════════════════════════════════════════════════════════════════
  if (memmap_request.response == NULL || hhdm_request.response == NULL) {
    klog_puts("[ERR] Missing Limine memory map or HHDM responses. Halting.\n");
    halt();
  }

  pmm_init(memmap_request.response, hhdm_request.response->offset);

  klog_puts("[OK] Physical Memory Manager (PMM) Initialized.\n");
  klog_puts("     Total RAM:  ");
  klog_uint64(pmm_get_total_memory() / (1024 * 1024));
  klog_puts(" MB\n");

  klog_puts("     Usable RAM: ");
  klog_uint64(pmm_get_usable_memory() / (1024 * 1024));
  klog_puts(" MB\n\n");

  klog_puts("[OK] Reclaiming Limine Bootloader Memory...\n");
  pmm_reclaim_bootloader();
  klog_puts("     Optimized Usable RAM: ");
  klog_uint64(pmm_get_usable_memory() / (1024 * 1024));
  klog_puts(" MB\n\n");

  klog_puts("[OK] Initializing Virtual Memory Manager (VMM)...\n");
  vmm_init();
  klog_puts("     Active CR3 Page Map hooked.\n");

  klog_puts("[OK] Testing VMM mapping... (0xCAFEBABE000)\n");
  void *test_phys = pmm_alloc();
  if (test_phys) {
    uint64_t vaddr = 0xCAFEBABE000;
    if (!vmm_map_page(vmm_get_active_pml4(), vaddr, (uint64_t)test_phys,
                      PAGE_FLAG_RW | PAGE_FLAG_USER)) {
      klog_puts("[ERROR] vmm_map_page test failed\n");
    } else {
      volatile uint64_t *test_ptr = (volatile uint64_t *)vaddr;
      *test_ptr = 0x1337BEEF; // If this page faults, the mapping failed!

      if (*test_ptr == 0x1337BEEF) {
        klog_puts("     VMM custom mapping test SUCCESSFUL!\n");
      } else {
        klog_puts("     VMM custom mapping test FAILED!\n");
      }
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

  // ═══════════════════════════════════════════════════════════════════════
  //  Phase 5: Multitasking & CPU Initialization
  // ═══════════════════════════════════════════════════════════════════════
  cpu_init();

  klog_puts("[INFO] Initializing Scheduler...\n");
  sched_init();

  if (lapic_base && ioapic_base) {
    klog_puts("\n[INFO] Switching to APIC interrupt mode...\n");

    // Disable interrupts during the transition
    __asm__ volatile("cli");

    // ── 5a. Disable the legacy 8259 PIC ─────────────────────────────────
    pic_disable();
    klog_puts("[OK] Legacy 8259 PIC disabled.\n");

    // ── 5b. Initialize the Local APIC ───────────────────────────────────
    lapic_init((uint64_t)lapic_base);

    // ── 5c. Initialize the I/O APIC ─────────────────────────────────────
    ioapic_init((uint64_t)ioapic_base, acpi_get_ioapic_gsi_base());

    // ── 5d. Route PIT (IRQ 0) through the I/O APIC ─────────────────────
    uint32_t pit_gsi = 0;
    uint16_t pit_flags = 0;
    acpi_get_irq_override(0, &pit_gsi, &pit_flags);
    ioapic_route_irq((uint8_t)pit_gsi, 32, (uint8_t)lapic_get_id(), pit_flags);
    klog_puts("[OK] PIT routed: GSI ");
    {
      char c = '0' + (char)pit_gsi;
      klog_putchar(c);
    }
    klog_puts(" -> Vector 32\n");

    // ── 5e. Route Keyboard (IRQ 1) through the I/O APIC ────────────────
    uint32_t kbd_gsi = 1;
    uint16_t kbd_flags = 0;
    acpi_get_irq_override(1, &kbd_gsi, &kbd_flags);
    ioapic_route_irq((uint8_t)kbd_gsi, 33, (uint8_t)lapic_get_id(), kbd_flags);
    klog_puts("[OK] Keyboard routed: GSI ");
    {
      char c = '0' + (char)kbd_gsi;
      klog_putchar(c);
    }
    klog_puts(" -> Vector 33\n");

    // ── 5f. Switch ISR EOI routing to LAPIC ─────────────────────────────
    isr_set_apic_mode(true);

    // Re-enable interrupts — now handled through the APIC path
    __asm__ volatile("sti");

    klog_puts("[OK] APIC interrupt mode ACTIVE.\n\n");

    // ── 5h. Start the LAPIC timer (calibrates against PIT) ──────────────
    lapic_timer_init();

    // ── 5i. Wake up Application Processors ──────────────────────────────
    // This is done AFTER lapic_timer_init because APs need the calibrated
    // ticks_per_ms value to initialize their own timers.
    cpu_init_aps();
  } else {
    klog_puts(
        "[WARN] APIC hardware not detected — staying with legacy PIC.\n\n");
  }

  // ═══════════════════════════════════════════════════════════════════════
  //  Phase 6: Kernel heap
  // ═══════════════════════════════════════════════════════════════════════
  klog_puts("[OK] Initializing Kernel Heap...\n");
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
    klog_puts(heap_test);
    kfree(heap_test);
  } else {
    klog_puts("     Heap allocation FAILED!\n");
  }

  // ═══════════════════════════════════════════════════════════════════════
  //  Phase 6.5: PCI Enumeration & Disk Drivers
  // ═══════════════════════════════════════════════════════════════════════
  // Initialize Virtual Filesystem and Ramfs
  klog_puts("[OK] Initializing RamFS & Virtual Filesystem (VFS)...\n");
  ramfs_init();
  fb_register_vfs();

  pci_init();
  ahci_init();
  rtl8139_init();
  net_init();

  int devs = block_count();
  klog_puts("[DIAG] Available Block Devices (via Block API):\n");
  for (int i = 0; i < devs; i++) {
    struct block_device *dev = block_get(i);
    if (!dev)
      continue;
    klog_puts("   > ");
    klog_puts(dev->name);
    klog_puts("\n");
  }

  // Mount ext2 as root filesystem
  struct block_device *disk = block_get(0);
  if (disk) {
    if (ext2_mount_root(disk) == 0) {
      // Create /dev and /tmp on ext2 if they don't exist
      vfs_node_t *dev_dir = vfs_finddir(fs_root, "dev");
      if (!dev_dir) {
        vfs_mkdir(fs_root, "dev", 0755);
        dev_dir = vfs_finddir(fs_root, "dev");
      } else {
        kfree(dev_dir);
        dev_dir = vfs_finddir(fs_root, "dev");
      }
      vfs_node_t *tmp_dir = vfs_finddir(fs_root, "tmp");
      if (!tmp_dir) {
        vfs_mkdir(fs_root, "tmp", 0777);
      } else {
        kfree(tmp_dir);
      }
      // Re-register block devices to the new /dev
      block_repopulate_devices();
      // Re-register framebuffer and console devices
      fb_register_vfs();
    }
  } else {
    klog_puts("[WARN] No block device found for ext2 root mount.\n");
  }

  // ═══════════════════════════════════════════════════════════════════════
  //  Phase 7: Interactive shell
  // ═══════════════════════════════════════════════════════════════════════
  klog_puts("\nKernel initialization complete. Starting shell...\n");
  shell_init();
  shell_run();
}
