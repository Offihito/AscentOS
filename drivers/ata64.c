// ata64.c — AscentOS 64-bit ATA PIO Disk Sürücüsü (LBA48)
//
// Ext2 filesystem katmanı (ext2.c) için ham sektör okuma/yazma sağlar.
// Tüm I/O LBA48 protokolü ile yapılır: 2^48 sektör = 128 PiB adreslenebilir.
//
// Dışa açık API:
//   disk_read_sector64     (uint32_t lba)  — Ext2 / genel kullanım
//   disk_write_sector64    (uint32_t lba)  — Ext2 / genel kullanım
//   disk_read_sector64_ext (uint64_t lba)  — >137 GB diskler için
//   disk_write_sector64_ext(uint64_t lba)  — >137 GB diskler için

#include "ata64.h"
#include <stdint.h>
#include <stddef.h>

// ============================================================
//  ATA PIO port map (primary channel, master drive)
// ============================================================
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECTOR_CNT  0x1F2   // also SECTOR_CNT_HI (HOB)
#define ATA_LBA_LO      0x1F3   // also LBA_LO_HI  (HOB)
#define ATA_LBA_MID     0x1F4   // also LBA_MID_HI (HOB)
#define ATA_LBA_HI      0x1F5   // also LBA_HI_HI  (HOB)
#define ATA_DEVICE      0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7
#define ATA_DEV_CTRL    0x3F6   // Device Control / Alt Status

// LBA48 komutları
#define ATA_CMD_READ_EXT    0x24  // READ SECTORS EXT  (LBA48)
#define ATA_CMD_WRITE_EXT   0x34  // WRITE SECTORS EXT (LBA48)
#define ATA_CMD_FLUSH_EXT   0xEA  // FLUSH CACHE EXT

// LBA28 (referans için — kullanılmıyor)
#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30
#define ATA_CMD_FLUSH   0xE7

#define ATA_SR_BSY      0x80
#define ATA_SR_DRQ      0x08
#define ATA_SR_ERR      0x01

// ============================================================
//  I/O yardımcıları
// ============================================================
static inline uint8_t inb_p(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outb_p(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint16_t inw_p(uint16_t port) {
    uint16_t v;
    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw_p(uint16_t port, uint16_t v) {
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}

// Timeout: ~500ms (döngü başına ~1 ns varsayımıyla)
#define ATA_TIMEOUT 500000000u

static int ata_wait_bsy_timeout(void) {
    for (uint32_t i = 0; i < ATA_TIMEOUT; i++) {
        if (!(inb_p(ATA_STATUS) & ATA_SR_BSY)) return 1;
    }
    return 0;
}
static int ata_wait_drq_timeout(void) {
    for (uint32_t i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t st = inb_p(ATA_STATUS);
        if (st & ATA_SR_ERR) return 0;
        if (st & ATA_SR_DRQ) return 1;
    }
    return 0;
}

// ============================================================
//  LBA48 ile TEK sektör oku / yaz
//
//  Protokol:
//   1. Device register: 0x40 (LBA modu, master)
//   2. HOB pass: sector count HIGH + LBA[47:24]
//   3. LOB pass: sector count LOW  + LBA[23:0]
//   4. Komut gönder, DRQ bekle, 256 word transfer et
// ============================================================
static int ata_read_lba48(uint64_t lba, uint8_t* buf) {
    if (!ata_wait_bsy_timeout()) return 0;

    outb_p(ATA_DEVICE, 0x40);

    // HOB pass (high bytes)
    outb_p(ATA_SECTOR_CNT, 0);
    outb_p(ATA_LBA_LO,  (uint8_t)(lba >> 24));
    outb_p(ATA_LBA_MID, (uint8_t)(lba >> 32));
    outb_p(ATA_LBA_HI,  (uint8_t)(lba >> 40));

    // LOB pass (low bytes)
    outb_p(ATA_SECTOR_CNT, 1);
    outb_p(ATA_LBA_LO,  (uint8_t)(lba));
    outb_p(ATA_LBA_MID, (uint8_t)(lba >>  8));
    outb_p(ATA_LBA_HI,  (uint8_t)(lba >> 16));

    outb_p(ATA_COMMAND, ATA_CMD_READ_EXT);

    if (!ata_wait_drq_timeout()) return 0;
    for (int i = 0; i < 256; i++)
        ((uint16_t*)buf)[i] = inw_p(ATA_DATA);

    if (inb_p(ATA_STATUS) & ATA_SR_ERR) return 0;
    return 1;
}

static int ata_write_lba48(uint64_t lba, const uint8_t* buf) {
    if (!ata_wait_bsy_timeout()) return 0;

    outb_p(ATA_DEVICE, 0x40);

    // HOB pass
    outb_p(ATA_SECTOR_CNT, 0);
    outb_p(ATA_LBA_LO,  (uint8_t)(lba >> 24));
    outb_p(ATA_LBA_MID, (uint8_t)(lba >> 32));
    outb_p(ATA_LBA_HI,  (uint8_t)(lba >> 40));

    // LOB pass
    outb_p(ATA_SECTOR_CNT, 1);
    outb_p(ATA_LBA_LO,  (uint8_t)(lba));
    outb_p(ATA_LBA_MID, (uint8_t)(lba >>  8));
    outb_p(ATA_LBA_HI,  (uint8_t)(lba >> 16));

    outb_p(ATA_COMMAND, ATA_CMD_WRITE_EXT);

    if (!ata_wait_drq_timeout()) return 0;
    for (int i = 0; i < 256; i++)
        outw_p(ATA_DATA, ((const uint16_t*)buf)[i]);

    outb_p(ATA_COMMAND, ATA_CMD_FLUSH_EXT);
    if (!ata_wait_bsy_timeout()) return 0;

    if (inb_p(ATA_STATUS) & ATA_SR_ERR) return 0;
    return 1;
}

// ============================================================
//  Dışa açık API
// ============================================================

// uint32_t LBA wrapper — Ext2 ve genel kullanım için
int disk_read_sector64(uint32_t lba, uint8_t* buf) {
    return ata_read_lba48((uint64_t)lba, buf);
}
int disk_write_sector64(uint32_t lba, const uint8_t* buf) {
    return ata_write_lba48((uint64_t)lba, buf);
}

// uint64_t LBA wrapper — >137 GB (2 TiB üstü) diskler için
int disk_read_sector64_ext(uint64_t lba, uint8_t* buf) {
    return ata_read_lba48(lba, buf);
}
int disk_write_sector64_ext(uint64_t lba, const uint8_t* buf) {
    return ata_write_lba48(lba, buf);
}