#ifndef PCI_PCI_H
#define PCI_PCI_H

#include <stdint.h>
#include <stdbool.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_MAX_DEVICES 64

struct pci_device {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
    uint32_t bar[6];
    uint8_t  irq_line;
};

// Read/write PCI configuration space
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

// Initialize PCI and enumerate all devices
void pci_init(void);

// Find a device by class/subclass. Returns NULL if not found.
struct pci_device *pci_find_device(uint8_t class_code, uint8_t subclass);

// Get the number of discovered devices
uint32_t pci_get_device_count(void);

// Get device by index
struct pci_device *pci_get_device(uint32_t index);

#endif
