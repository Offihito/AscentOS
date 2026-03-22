#include "pci.h"

// ============================================================================
// Port I/O yardımcıları (sadece bu dosyada kullanılır)
// ============================================================================
static inline void _outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t _inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// ============================================================================
// Forward declaration
// ============================================================================
static void _fill_device(uint8_t bus, uint8_t dev, uint8_t fn,
                         uint16_t vid, uint16_t did, PCIDevice* out);

// ============================================================================
// Temel PCI I/O
// ============================================================================

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  <<  8)
                  | (reg & 0xFC);
    _outl(PCI_CONFIG_ADDR, addr);
    return _inl(PCI_CONFIG_DATA);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val) {
    uint32_t addr = (1u << 31)
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  <<  8)
                  | (reg & 0xFC);
    _outl(PCI_CONFIG_ADDR, addr);
    _outl(PCI_CONFIG_DATA, val);
}

// ============================================================================
// Yardımcı Fonksiyonlar
// ============================================================================

void pci_enable_busmaster(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t cmd = pci_read32(bus, dev, fn, PCI_REG_COMMAND);
    cmd |= PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER;
    pci_write32(bus, dev, fn, PCI_REG_COMMAND, cmd);
}

bool pci_find_device(uint16_t vendor_id, uint16_t device_id, PCIDevice* out) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read32((uint8_t)bus, dev, 0, PCI_REG_VENDOR_DEVICE);
            if (id == 0xFFFFFFFF) continue;

            uint16_t vid = (uint16_t)(id & 0xFFFF);
            uint16_t did = (uint16_t)(id >> 16);

            if (vid == vendor_id && did == device_id) {
                _fill_device((uint8_t)bus, dev, 0, vid, did, out);
                return true;
            }
        }
    }
    return false;
}

// ============================================================================
// Ortak cihaz doldurma yardımcısı
// ============================================================================
static void _fill_device(uint8_t bus, uint8_t dev, uint8_t fn,
                          uint16_t vid, uint16_t did, PCIDevice* out) {
    out->bus       = bus;
    out->dev       = dev;
    out->fn        = fn;
    out->vendor_id = vid;
    out->device_id = did;

    uint32_t bar0 = pci_read32(bus, dev, fn, PCI_REG_BAR0);
    out->io_base = (uint16_t)(bar0 & 0xFFFC);

    uint32_t irq_reg = pci_read32(bus, dev, fn, PCI_REG_IRQ_LINE);
    out->irq = (uint8_t)(irq_reg & 0xFF);

    uint32_t class_reg = pci_read32(bus, dev, fn, PCI_REG_CLASS_REV);
    out->class_code = (uint8_t)((class_reg >> 24) & 0xFF);
    out->subclass   = (uint8_t)((class_reg >> 16) & 0xFF);
}

bool pci_find_device_next(uint16_t vendor_id, uint16_t device_id,
                          const PCIDevice* prev, PCIDevice* out) {
    // prev'in konumundan bir sonraki cihazdan aramaya başla.
    // Aynı bus+dev'de fn>0 varsa ondan devam et, yoksa dev+1'den.
    uint16_t start_bus = prev->bus;
    uint8_t  start_dev = prev->dev;
    uint8_t  start_fn  = prev->fn + 1; // bir sonraki function

    for (uint16_t bus = start_bus; bus < 256; bus++) {
        uint8_t dev_begin = (bus == start_bus) ? start_dev : 0;

        for (uint8_t dev = dev_begin; dev < 32; dev++) {
            // Multi-function kontrolü: header type bit7
            uint32_t hdr = pci_read32((uint8_t)bus, dev, 0, PCI_REG_HEADER_TYPE);
            uint8_t  header_type = (uint8_t)((hdr >> 16) & 0xFF);
            uint8_t  max_fn = (header_type & 0x80) ? 8 : 1;

            uint8_t fn_begin = (bus == start_bus && dev == start_dev) ? start_fn : 0;

            for (uint8_t fn = fn_begin; fn < max_fn; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, dev, fn, PCI_REG_VENDOR_DEVICE);
                if (id == 0xFFFFFFFF) continue;

                uint16_t vid = (uint16_t)(id & 0xFFFF);
                uint16_t did = (uint16_t)(id >> 16);

                if (vid == vendor_id && did == device_id) {
                    _fill_device((uint8_t)bus, dev, fn, vid, did, out);
                    return true;
                }
            }

            // Bir sonraki bus/dev için start_fn sıfırla
            start_fn = 0;
        }
        start_dev = 0;
    }
    return false;
}

// ============================================================================
// BAR Okuma
// ============================================================================

PCIBAR pci_read_bar(const PCIDevice* device, uint8_t bar_index) {
    PCIBAR result = { PCI_BAR_TYPE_INVALID, 0, 0, 0 };

    if (bar_index > 5) return result;

    uint8_t  bus = device->bus;
    uint8_t  dev = device->dev;
    uint8_t  fn  = device->fn;
    uint8_t  reg = (uint8_t)(PCI_REG_BAR0 + bar_index * 4);

    uint32_t bar_val = pci_read32(bus, dev, fn, reg);

    // Boş BAR
    if (bar_val == 0x00000000 || bar_val == 0xFFFFFFFF) return result;

    // ── I/O Space BAR (bit 0 = 1) ────────────────────────────────────────
    if (bar_val & 0x1) {
        result.type    = PCI_BAR_TYPE_IO;
        result.address = (uint64_t)(bar_val & 0xFFFFFFFC);

        // Boyutu bulmak için tüm 1 yaz, geri oku, NOT+1 al
        // Önce Command register'daki I/O Space bitini kapat
        uint32_t cmd = pci_read32(bus, dev, fn, PCI_REG_COMMAND);
        pci_write32(bus, dev, fn, PCI_REG_COMMAND, cmd & ~PCI_CMD_IO_SPACE);

        pci_write32(bus, dev, fn, reg, 0xFFFFFFFF);
        uint32_t size_mask = pci_read32(bus, dev, fn, reg);
        pci_write32(bus, dev, fn, reg, bar_val);         // orijinal değeri geri yaz

        pci_write32(bus, dev, fn, PCI_REG_COMMAND, cmd); // Command'ı geri aç

        size_mask &= 0xFFFFFFFC;
        result.size = (uint32_t)(~size_mask + 1) & 0xFFFF;

        return result;
    }

    // ── Memory Space BAR (bit 0 = 0) ─────────────────────────────────────
    uint8_t mem_type     = (uint8_t)((bar_val >> 1) & 0x3);
    uint8_t prefetchable = (uint8_t)((bar_val >> 3) & 0x1);

    result.prefetchable = prefetchable;

    // Önce Command register'daki Mem Space bitini kapat (boyut ölçümü için)
    uint32_t cmd = pci_read32(bus, dev, fn, PCI_REG_COMMAND);
    pci_write32(bus, dev, fn, PCI_REG_COMMAND, cmd & ~PCI_CMD_MEM_SPACE);

    if (mem_type == 0x0) {
        // 32-bit MMIO
        result.type    = PCI_BAR_TYPE_MEM32;
        result.address = (uint64_t)(bar_val & 0xFFFFFFF0);

        pci_write32(bus, dev, fn, reg, 0xFFFFFFFF);
        uint32_t size_mask = pci_read32(bus, dev, fn, reg);
        pci_write32(bus, dev, fn, reg, bar_val);

        size_mask &= 0xFFFFFFF0;
        if (size_mask) result.size = (uint32_t)(~size_mask + 1);

    } else if (mem_type == 0x2) {
        // 64-bit MMIO — bu BAR ve bir sonraki BAR birlikte kullanılır
        if (bar_index >= 5) {
            // BAR5 tek başına 64-bit olamaz (BAR6 yok)
            pci_write32(bus, dev, fn, PCI_REG_COMMAND, cmd);
            return result;
        }

        result.type = PCI_BAR_TYPE_MEM64;

        uint8_t  reg_hi  = (uint8_t)(reg + 4);
        uint32_t bar_hi  = pci_read32(bus, dev, fn, reg_hi);

        result.address = ((uint64_t)bar_hi << 32) | (uint64_t)(bar_val & 0xFFFFFFF0);

        // Boyutu düşük 32 bit üzerinden ölç (yeterince kesin)
        pci_write32(bus, dev, fn, reg, 0xFFFFFFFF);
        uint32_t size_mask = pci_read32(bus, dev, fn, reg);
        pci_write32(bus, dev, fn, reg, bar_val);

        size_mask &= 0xFFFFFFF0;
        if (size_mask) result.size = (uint32_t)(~size_mask + 1);
    }
    // mem_type == 0x1 (20-bit, below 1MB) artık kullanılmıyor, INVALID kalır

    pci_write32(bus, dev, fn, PCI_REG_COMMAND, cmd); // Command'ı geri aç

    return result;
}

// ============================================================================
// Vendor İsimleri
// ============================================================================

const char* pci_vendor_name(uint16_t vid) {
    switch (vid) {
        case 0x8086: return "Intel";
        case 0x1022: return "AMD";
        case 0x10DE: return "NVIDIA";
        case 0x1234: return "QEMU/Bochs";
        case 0x10EC: return "Realtek";
        case 0x1AF4: return "VirtIO/Red Hat";
        case 0x14E4: return "Broadcom";
        case 0x168C: return "Qualcomm Atheros";
        case 0x1B36: return "QEMU";
        case 0x1AE0: return "Google";
        case 0x15AD: return "VMware";
        case 0x80EE: return "VirtualBox";
        default:     return "Unknown";
    }
}

// ============================================================================
// Class İsimleri
// ============================================================================

const char* pci_class_name(uint8_t class_code, uint8_t subclass) {
    if (class_code == 0x00) return "Unclassified";
    if (class_code == 0x01) {
        if (subclass == 0x01) return "IDE Controller";
        if (subclass == 0x06) return "SATA Controller (AHCI)";
        if (subclass == 0x80) return "Storage Controller";
        return "Mass Storage";
    }
    if (class_code == 0x02) {
        if (subclass == 0x00) return "Ethernet Controller";
        if (subclass == 0x80) return "Network Controller";
        return "Network";
    }
    if (class_code == 0x03) {
        if (subclass == 0x00) return "VGA Controller";
        if (subclass == 0x01) return "XGA Controller";
        if (subclass == 0x02) return "3D Controller";
        return "Display";
    }
    if (class_code == 0x04) return "Multimedia";
    if (class_code == 0x05) return "Memory Controller";
    if (class_code == 0x06) {
        if (subclass == 0x00) return "Host Bridge";
        if (subclass == 0x01) return "ISA Bridge";
        if (subclass == 0x04) return "PCI-PCI Bridge";
        if (subclass == 0x80) return "Bridge";
        return "Bridge";
    }
    if (class_code == 0x07) return "Communication";
    if (class_code == 0x08) {
        if (subclass == 0x00) return "PIC";
        if (subclass == 0x01) return "DMA Controller";
        if (subclass == 0x02) return "Timer";
        if (subclass == 0x03) return "RTC";
        return "System Peripheral";
    }
    if (class_code == 0x09) return "Input Device";
    if (class_code == 0x0A) return "Docking Station";
    if (class_code == 0x0B) return "Processor";
    if (class_code == 0x0C) {
        if (subclass == 0x03) return "USB Controller";
        if (subclass == 0x05) return "SMBus";
        return "Serial Bus";
    }
    if (class_code == 0x0D) return "Wireless";
    if (class_code == 0x0F) return "Satellite Communication";
    if (class_code == 0x10) return "Encryption";
    if (class_code == 0x11) return "Signal Processing";
    if (class_code == 0xFF) return "Unassigned";
    return "Other";
}