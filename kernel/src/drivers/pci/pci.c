#include "drivers/pci/pci.h"
#include "console/console.h"
#include "io/io.h"
#include <stddef.h>

static struct pci_device devices[PCI_MAX_DEVICES];
static uint32_t device_count = 0;

// ── PCI Config Space Access ─────────────────────────────────────────────────

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset) {
  uint32_t address = (1u << 31) | ((uint32_t)bus << 16) |
                     ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                     (offset & 0xFC);
  outl(PCI_CONFIG_ADDRESS, address);
  return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset,
                        uint32_t value) {
  uint32_t address = (1u << 31) | ((uint32_t)bus << 16) |
                     ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                     (offset & 0xFC);
  outl(PCI_CONFIG_ADDRESS, address);
  outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func,
                           uint8_t offset) {
  uint32_t val = pci_config_read32(bus, slot, func, offset & 0xFC);
  return (uint16_t)(val >> ((offset & 2) * 8));
}

// ── Helpers ─────────────────────────────────────────────────────────────────

static void print_hex8(uint8_t val) {
  const char *hex = "0123456789ABCDEF";
  console_putchar(hex[(val >> 4) & 0xF]);
  console_putchar(hex[val & 0xF]);
}

static void print_hex16(uint16_t val) {
  print_hex8((uint8_t)(val >> 8));
  print_hex8((uint8_t)(val & 0xFF));
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
  while (i > 0) {
    console_putchar(buf[--i]);
  }
}

// ── Device scanning ─────────────────────────────────────────────────────────

static void pci_check_function(uint8_t bus, uint8_t slot, uint8_t func) {
  uint32_t reg0 = pci_config_read32(bus, slot, func, 0x00);
  uint16_t vendor_id = reg0 & 0xFFFF;
  uint16_t device_id = reg0 >> 16;

  if (vendor_id == 0xFFFF)
    return; // No device
  if (device_count >= PCI_MAX_DEVICES)
    return;

  uint32_t reg2 = pci_config_read32(bus, slot, func, 0x08);
  uint32_t reg3 = pci_config_read32(bus, slot, func, 0x0C);
  uint32_t reg_irq = pci_config_read32(bus, slot, func, 0x3C);

  struct pci_device *dev = &devices[device_count];
  dev->bus = bus;
  dev->slot = slot;
  dev->func = func;
  dev->vendor_id = vendor_id;
  dev->device_id = device_id;
  dev->class_code = (reg2 >> 24) & 0xFF;
  dev->subclass = (reg2 >> 16) & 0xFF;
  dev->prog_if = (reg2 >> 8) & 0xFF;
  dev->header_type = (reg3 >> 16) & 0xFF;
  dev->irq_line = reg_irq & 0xFF;

  // Read BARs (only for standard header type 0x00)
  if ((dev->header_type & 0x7F) == 0x00) {
    for (int i = 0; i < 6; i++) {
      dev->bar[i] = pci_config_read32(bus, slot, func, 0x10 + i * 4);
    }
  }

  device_count++;
}

static void pci_check_device(uint8_t bus, uint8_t slot) {
  uint32_t reg0 = pci_config_read32(bus, slot, 0, 0x00);
  uint16_t vendor_id = reg0 & 0xFFFF;
  if (vendor_id == 0xFFFF)
    return;

  pci_check_function(bus, slot, 0);

  // Check if multi-function device
  uint32_t reg3 = pci_config_read32(bus, slot, 0, 0x0C);
  uint8_t header_type = (reg3 >> 16) & 0xFF;
  if (header_type & 0x80) {
    for (uint8_t func = 1; func < 8; func++) {
      pci_check_function(bus, slot, func);
    }
  }
}

// ── Public API ──────────────────────────────────────────────────────────────

void pci_init(void) {
  device_count = 0;
  console_puts("[INFO] Scanning PCI bus...\n");

  for (uint16_t bus = 0; bus < 256; bus++) {
    for (uint8_t slot = 0; slot < 32; slot++) {
      pci_check_device((uint8_t)bus, slot);
    }
  }

  console_puts("[OK] PCI: Found ");
  print_uint32(device_count);
  console_puts(" device(s):\n");

  for (uint32_t i = 0; i < device_count; i++) {
    struct pci_device *dev = &devices[i];
    console_puts("     ");
    print_uint32(dev->bus);
    console_putchar(':');
    print_uint32(dev->slot);
    console_putchar('.');
    print_uint32(dev->func);
    console_puts(" [");
    print_hex8(dev->class_code);
    console_putchar(':');
    print_hex8(dev->subclass);
    console_puts("] vendor=0x");
    print_hex16(dev->vendor_id);
    console_puts(" device=0x");
    print_hex16(dev->device_id);
    console_putchar('\n');
  }
}

struct pci_device *pci_find_device(uint8_t class_code, uint8_t subclass) {
  for (uint32_t i = 0; i < device_count; i++) {
    if (devices[i].class_code == class_code &&
        devices[i].subclass == subclass) {
      return &devices[i];
    }
  }
  return NULL;
}

struct pci_device *pci_find_device_by_id(uint16_t vendor_id,
                                         uint16_t device_id) {
  for (uint32_t i = 0; i < device_count; i++) {
    if (devices[i].vendor_id == vendor_id &&
        devices[i].device_id == device_id) {
      return &devices[i];
    }
  }
  return NULL;
}

void pci_enable_bus_mastering(struct pci_device *dev) {
  uint32_t cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
  cmd |= (1 << 2); // Set Bus Master bit
  pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

uint32_t pci_get_device_count(void) { return device_count; }

struct pci_device *pci_get_device(uint32_t index) {
  if (index >= device_count)
    return NULL;
  return &devices[index];
}
