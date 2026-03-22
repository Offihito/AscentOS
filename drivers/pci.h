#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
//  PCI Configuration Port Addresses
// ============================================================================
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

#define PCI_REG_VENDOR_DEVICE   0x00
#define PCI_REG_COMMAND         0x04
#define PCI_REG_CLASS_REV       0x08
#define PCI_REG_HEADER_TYPE     0x0C
#define PCI_REG_BAR0            0x10
#define PCI_REG_BAR1            0x14
#define PCI_REG_BAR2            0x18
#define PCI_REG_BAR3            0x1C
#define PCI_REG_BAR4            0x20
#define PCI_REG_BAR5            0x24
#define PCI_REG_IRQ_LINE        0x3C

#define PCI_CMD_IO_SPACE        (1 << 0)
#define PCI_CMD_MEM_SPACE       (1 << 1)
#define PCI_CMD_BUS_MASTER      (1 << 2)

// ============================================================================
// BAR Types
// ============================================================================
#define PCI_BAR_TYPE_IO         0   
#define PCI_BAR_TYPE_MEM32      1   
#define PCI_BAR_TYPE_MEM64      2   
#define PCI_BAR_TYPE_INVALID    3   

typedef struct {
    uint8_t  type;        
    uint64_t address;      
    uint32_t size;          
    uint8_t  prefetchable;  
} PCIBAR;

// ============================================================================
// PCI Device identifier
// ============================================================================
typedef struct {
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  fn;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint16_t io_base;   
    uint8_t  irq;
} PCIDevice;

// ============================================================================
// Basic PCI I/O
// ============================================================================

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val);

// ============================================================================
// Helper Functions
// ============================================================================

void pci_enable_busmaster(uint8_t bus, uint8_t dev, uint8_t fn);

bool pci_find_device(uint16_t vendor_id, uint16_t device_id, PCIDevice* out);

bool pci_find_device_next(uint16_t vendor_id, uint16_t device_id,
                          const PCIDevice* prev, PCIDevice* out);

PCIBAR pci_read_bar(const PCIDevice* dev, uint8_t bar_index);

const char* pci_vendor_name(uint16_t vid);
const char* pci_class_name(uint8_t class_code, uint8_t subclass);

#endif 