/*
 * UHCI (Universal Host Controller Interface) — USB 1.x Driver
 *
 * Phase 1: PCI Discovery & Controller Skeleton
 *
 * This phase discovers all UHCI controllers on the PCI bus, extracts their
 * I/O base addresses and IRQ lines, silences the hardware (stops the
 * schedule, disables interrupts), and provides a self-test that verifies
 * register access and enumerates root hub port status.
 */

#include "drivers/usb/uhci.h"
#include "console/console.h"
#include "console/klog.h"
#include "drivers/pci/pci.h"
#include "io/io.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ── Private state ───────────────────────────────────────────────────────────

static struct uhci_controller controllers[UHCI_MAX_CONTROLLERS];
static int controller_count = 0;

// ── Helpers ─────────────────────────────────────────────────────────────────

static void print_hex16(uint16_t val) {
  const char *hex = "0123456789ABCDEF";
  console_putchar(hex[(val >> 12) & 0xF]);
  console_putchar(hex[(val >> 8) & 0xF]);
  console_putchar(hex[(val >> 4) & 0xF]);
  console_putchar(hex[val & 0xF]);
}

static void print_hex32(uint32_t val) {
  print_hex16((uint16_t)(val >> 16));
  print_hex16((uint16_t)(val & 0xFFFF));
}

static void print_uint32(uint32_t num) {
  if (num == 0) {
    console_putchar('0');
    return;
  }
  char buf[10];
  int i = 0;
  while (num > 0) {
    buf[i++] = '0' + (num % 10);
    num /= 10;
  }
  while (i > 0)
    console_putchar(buf[--i]);
}

// ── UHCI register I/O wrappers ──────────────────────────────────────────────

static inline uint16_t uhci_read16(struct uhci_controller *hc, uint16_t reg) {
  return inw(hc->io_base + reg);
}

static inline void uhci_write16(struct uhci_controller *hc, uint16_t reg,
                                uint16_t val) {
  outw(hc->io_base + reg, val);
}

static inline uint32_t uhci_read32(struct uhci_controller *hc, uint16_t reg) {
  return inl(hc->io_base + reg);
}

static inline void uhci_write32(struct uhci_controller *hc, uint16_t reg,
                                uint32_t val) {
  outl(hc->io_base + reg, val);
}

static inline uint8_t uhci_read8(struct uhci_controller *hc, uint16_t reg) {
  return inb(hc->io_base + reg);
}

// ── Controller silence ──────────────────────────────────────────────────────
// Ensure the controller is stopped and won't fire spurious IRQs before we're
// ready. This is critical during early boot when IRQ routing may not be set up.

static void uhci_silence(struct uhci_controller *hc) {
  // 0. Disable PCI INTx at the PCI config level.
  uint32_t pci_cmd =
      pci_config_read32(hc->pci_bus, hc->pci_slot, hc->pci_func, 0x04);
  pci_cmd |= (1 << 10); // PCI Command: Interrupt Disable
  pci_config_write32(hc->pci_bus, hc->pci_slot, hc->pci_func, 0x04, pci_cmd);

  // 1. More aggressive reset: Toggle Global Reset and Host Controller Reset
  //    This ensures the hardware is in a known clean state.
  uhci_write16(hc, UHCI_REG_USBCMD, UHCI_CMD_GRESET);
  for (int i = 0; i < 1000; i++)
    io_wait(); // Wait for reset to propagate
  uhci_write16(hc, UHCI_REG_USBCMD, 0x0000);
  for (int i = 0; i < 1000; i++)
    io_wait();

  uhci_write16(hc, UHCI_REG_USBCMD, UHCI_CMD_HCRESET);
  for (int i = 0; i < 1000; i++) {
    if (!(uhci_read16(hc, UHCI_REG_USBCMD) & UHCI_CMD_HCRESET))
      break;
    io_wait();
  }

  // 2. Disable all UHCI interrupts
  uhci_write16(hc, UHCI_REG_USBINTR, 0x0000);

  // 3. Clear any pending status bits (write-1-to-clear)
  uhci_write16(hc, UHCI_REG_USBSTS, 0xFFFF);

  // 4. Final confirmation of halt
  for (int i = 0; i < 1000; i++) {
    uint16_t s = uhci_read16(hc, UHCI_REG_USBSTS);
    uint16_t c = uhci_read16(hc, UHCI_REG_USBCMD);
    if (s & UHCI_STS_HCH)
      return;
    if (!(c & UHCI_CMD_RS) && s == 0x0000)
      return;
    io_wait();
  }

  klog_puts("[UHCI] Warning: controller did not halt cleanly\n");
}

// ── Detect number of root-hub ports ─────────────────────────────────────────
// UHCI spec says ports start at offset 0x10 and each is 2 bytes.
// We probe up to 8 ports; a non-existent port reads as 0xFFFF or has
// reserved bits set that real ports never show.

static uint8_t uhci_detect_ports(struct uhci_controller *hc) {
  uint8_t count = 0;
  for (int i = 0; i < 8; i++) {
    uint16_t portsc = inw(hc->io_base + UHCI_REG_PORTSC1 + (i * 2));
    // Bit 7 is always read as 1 on real UHCI ports
    if (!(portsc & (1 << 7)))
      break;
    // Bits 13-15 must be 0 on a valid port
    if (portsc & 0xE000)
      break;
    count++;
  }
  return count ? count : 2; // Default to 2 if detection fails
}

// ── PCI Discovery ───────────────────────────────────────────────────────────

static bool uhci_probe_pci_device(struct pci_device *pci) {
  if (controller_count >= UHCI_MAX_CONTROLLERS)
    return false;

  // UHCI uses I/O space — find the first BAR with bit 0 set (I/O indicator)
  uint16_t io_base = 0;
  for (int i = 0; i < 6; i++) {
    if ((pci->bar[i] & 0x01) && (pci->bar[i] & ~0x03) != 0) {
      io_base = (uint16_t)(pci->bar[i] & ~0x03);
      break;
    }
  }

  if (io_base == 0) {
    klog_puts("[UHCI] No I/O BAR found for controller at ");
    klog_hex32(pci->bus);
    klog_puts(":");
    klog_hex32(pci->slot);
    klog_puts("\n");
    return false;
  }

  struct uhci_controller *hc = &controllers[controller_count];
  hc->io_base = io_base;
  hc->irq_line = pci->irq_line;
  hc->pci_bus = pci->bus;
  hc->pci_slot = pci->slot;
  hc->pci_func = pci->func;
  hc->vendor_id = pci->vendor_id;
  hc->device_id = pci->device_id;
  hc->present = true;

  // Silence the controller before anything else
  uhci_silence(hc);

  // Detect how many root hub ports this controller has
  hc->num_ports = uhci_detect_ports(hc);

  controller_count++;
  return true;
}

// ── Public API ──────────────────────────────────────────────────────────────

void uhci_init(void) {
  controller_count = 0;
  console_puts("[INFO] Searching for UHCI (USB 1.x) controllers...\n");

  // ── First pass: silence ALL USB controllers (UHCI, EHCI, xHCI) ────────
  // Q35's ICH9 has EHCI (prog_if 0x20) in addition to UHCI.  Any of these
  // can assert level-triggered interrupts on shared IRQ lines, starving
  // other devices.  Disable PCI INTx for every USB controller we find.
  uint32_t pci_count = pci_get_device_count();
  for (uint32_t i = 0; i < pci_count; i++) {
    struct pci_device *dev = pci_get_device(i);
    if (!dev)
      continue;

    if (dev->class_code == PCI_CLASS_SERIAL_BUS &&
        dev->subclass == PCI_SUBCLASS_USB) {
      // Disable PCI INTx for this USB controller
      uint32_t cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
      cmd |= (1 << 10); // PCI Command: Interrupt Disable

      // For NON-UHCI controllers (like EHCI/xHCI), strictly disable them
      // by pulling the plug on Master, Memory, and I/O access.
      // We don't have drivers for them yet, so they shouldn't be active.
      if (dev->prog_if != PCI_PROGIF_UHCI) {
        cmd &= ~(0x07); // Clear Master, Memory, I/O
        klog_puts("[USB] Electrically disabled non-UHCI controller at PCI ");
        klog_hex32(dev->bus);
        klog_puts(":");
        klog_hex32(dev->slot);
        klog_puts(".");
        klog_hex32(dev->func);
        klog_puts("\n");
      }

      pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);

      // For UHCI controllers, disable Legacy Support (USB LEGSUP at 0xC0)
      // This is crucial to stop BIOS from interfering or firing SMIs.
      if (dev->prog_if == PCI_PROGIF_UHCI) {
        pci_config_write16(dev->bus, dev->slot, dev->func, 0xC0, 0x8F00);
      }
    }
  }

  // ── Second pass: probe UHCI-specific controllers ──────────────────────
  for (uint32_t i = 0; i < pci_count; i++) {
    struct pci_device *dev = pci_get_device(i);
    if (!dev)
      continue;

    // Match: Serial Bus Controller (0x0C) → USB (0x03) → UHCI (0x00)
    if (dev->class_code == PCI_CLASS_SERIAL_BUS &&
        dev->subclass == PCI_SUBCLASS_USB && dev->prog_if == PCI_PROGIF_UHCI) {
      uhci_probe_pci_device(dev);
    }
  }

  if (controller_count == 0) {
    console_puts("[WARN] No UHCI controllers found.\n");
    return;
  }

  console_puts("[OK] UHCI: Found ");
  print_uint32(controller_count);
  console_puts(" controller(s):\n");

  for (int i = 0; i < controller_count; i++) {
    struct uhci_controller *hc = &controllers[i];
    console_puts("     uhci");
    console_putchar('0' + i);
    console_puts(": PCI ");
    print_uint32(hc->pci_bus);
    console_putchar(':');
    print_uint32(hc->pci_slot);
    console_putchar('.');
    print_uint32(hc->pci_func);
    console_puts(" IO=0x");
    print_hex16(hc->io_base);
    console_puts(" IRQ=");
    print_uint32(hc->irq_line);
    console_puts(" ports=");
    print_uint32(hc->num_ports);
    console_putchar('\n');
  }
}

int uhci_get_controller_count(void) { return controller_count; }

struct uhci_controller *uhci_get_controller(int index) {
  if (index < 0 || index >= controller_count)
    return NULL;
  return &controllers[index];
}

// ── Self-Test ───────────────────────────────────────────────────────────────
// Verifies that we can read/write UHCI registers and dumps diagnostic state.

void uhci_self_test(void) {
  console_puts("\n[TEST] UHCI Self-Test\n");
  console_puts("─────────────────────────────────────────\n");

  if (controller_count == 0) {
    console_puts("  SKIP: No UHCI controllers present.\n");
    console_puts("─────────────────────────────────────────\n\n");
    return;
  }

  int pass = 0;
  int fail = 0;

  for (int i = 0; i < controller_count; i++) {
    struct uhci_controller *hc = &controllers[i];
    console_puts("  Controller ");
    console_putchar('0' + i);
    console_puts(" (IO=0x");
    print_hex16(hc->io_base);
    console_puts("):\n");

    // ── Test 1: HC should be stopped after uhci_silence() ───────────────
    //    Accept either HCH=1 (explicitly halted) or the "never started"
    //    state where USBCMD.RS=0 and USBSTS is clean (0x0000).
    uint16_t sts = uhci_read16(hc, UHCI_REG_USBSTS);
    uint16_t cmd_chk = uhci_read16(hc, UHCI_REG_USBCMD);
    bool halted =
        (sts & UHCI_STS_HCH) || (!(cmd_chk & UHCI_CMD_RS) && sts == 0x0000);
    if (halted) {
      console_puts("    [PASS] Controller is stopped (USBSTS=0x");
      print_hex16(sts);
      console_puts(")\n");
      pass++;
    } else {
      console_puts("    [FAIL] Controller NOT stopped (USBSTS=0x");
      print_hex16(sts);
      console_puts(")\n");
      fail++;
    }

    // ── Test 2: USBCMD should be 0 after silence ───────────────────────
    uint16_t cmd = uhci_read16(hc, UHCI_REG_USBCMD);
    if (cmd == 0x0000) {
      console_puts("    [PASS] USBCMD is cleared (0x");
      print_hex16(cmd);
      console_puts(")\n");
      pass++;
    } else {
      console_puts("    [FAIL] USBCMD not cleared (0x");
      print_hex16(cmd);
      console_puts(")\n");
      fail++;
    }

    // ── Test 3: USBINTR should be 0 (interrupts disabled) ──────────────
    uint16_t intr = uhci_read16(hc, UHCI_REG_USBINTR);
    if (intr == 0x0000) {
      console_puts("    [PASS] Interrupts disabled (USBINTR=0x");
      print_hex16(intr);
      console_puts(")\n");
      pass++;
    } else {
      console_puts("    [FAIL] Interrupts not disabled (USBINTR=0x");
      print_hex16(intr);
      console_puts(")\n");
      fail++;
    }

    // ── Test 4: SOF Modify register should default to 0x40 (64) ────────
    uint8_t sof = uhci_read8(hc, UHCI_REG_SOFMOD);
    if (sof == 0x40) {
      console_puts("    [PASS] SOFMOD default correct (0x40)\n");
      pass++;
    } else {
      console_puts("    [WARN] SOFMOD non-standard (0x");
      const char *hex = "0123456789ABCDEF";
      console_putchar(hex[(sof >> 4) & 0xF]);
      console_putchar(hex[sof & 0xF]);
      console_puts(")\n");
      // Not a hard fail — some chips differ
      pass++;
    }

    // ── Test 5: Frame List Base Address register is writable ────────────
    uint32_t old_fl = uhci_read32(hc, UHCI_REG_FLBASEADD);
    uhci_write32(hc, UHCI_REG_FLBASEADD, 0xDEAD0000);
    uint32_t new_fl = uhci_read32(hc, UHCI_REG_FLBASEADD);
    uhci_write32(hc, UHCI_REG_FLBASEADD, old_fl); // restore
    // Bottom 12 bits are always 0 (4K alignment), so expect 0xDEAD0000
    if ((new_fl & 0xFFFFF000) == 0xDEAD0000) {
      console_puts("    [PASS] FLBASEADD register is writable\n");
      pass++;
    } else {
      console_puts("    [FAIL] FLBASEADD read-back mismatch (0x");
      print_hex32(new_fl);
      console_puts(")\n");
      fail++;
    }

    // ── Test 6: Dump root hub port status ───────────────────────────────
    console_puts("    Port Status:\n");
    for (uint8_t p = 0; p < hc->num_ports; p++) {
      uint16_t portsc = uhci_read16(hc, UHCI_REG_PORTSC1 + (p * 2));
      console_puts("      Port ");
      console_putchar('1' + p);
      console_puts(": 0x");
      print_hex16(portsc);

      if (portsc & UHCI_PORT_CCS) {
        console_puts(" [CONNECTED");
        if (portsc & UHCI_PORT_LSDA)
          console_puts(", Low-Speed");
        else
          console_puts(", Full-Speed");
        console_putchar(']');
      } else {
        console_puts(" [EMPTY]");
      }

      if (portsc & UHCI_PORT_PE)
        console_puts(" ENABLED");

      console_putchar('\n');
    }
  }

  // ── Summary ─────────────────────────────────────────────────────────────
  console_puts("─────────────────────────────────────────\n");
  console_puts("  Results: ");
  print_uint32(pass);
  console_puts(" passed, ");
  print_uint32(fail);
  console_puts(" failed\n");

  if (fail == 0) {
    console_puts("  ✓ UHCI Phase 1 PASSED\n");
  } else {
    console_puts("  ✗ UHCI Phase 1 FAILED\n");
  }
  console_puts("─────────────────────────────────────────\n\n");
}
