#include "disk64.h"
#include <stdint.h>

// ATA port definitions
#define ATA_PRIMARY_DATA    0x1F0
#define ATA_PRIMARY_ERROR   0x1F1
#define ATA_PRIMARY_COUNT   0x1F2
#define ATA_PRIMARY_LBA_LOW 0x1F3
#define ATA_PRIMARY_LBA_MID 0x1F4
#define ATA_PRIMARY_LBA_HI  0x1F5
#define ATA_PRIMARY_DEVICE  0x1F6
#define ATA_PRIMARY_STATUS  0x1F7
#define ATA_PRIMARY_COMMAND 0x1F7

// I/O port functions for 64-bit
static inline uint8_t inb64(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb64(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw64(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw64(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

// Wait for disk to be ready
static void ata_wait_ready64(void) {
    while (inb64(ATA_PRIMARY_STATUS) & 0x80);  // BSY bit
}

static void ata_wait_drq64(void) {
    while (!(inb64(ATA_PRIMARY_STATUS) & 0x08));  // DRQ bit
}

// Read a sector from disk (LBA28 mode)
int disk_read_sector64(uint32_t lba, uint8_t* buffer) {
    ata_wait_ready64();
    
    // Select drive and set LBA
    outb64(ATA_PRIMARY_DEVICE, 0xE0 | ((lba >> 24) & 0x0F));
    outb64(ATA_PRIMARY_COUNT, 1);  // Read 1 sector
    outb64(ATA_PRIMARY_LBA_LOW, lba & 0xFF);
    outb64(ATA_PRIMARY_LBA_MID, (lba >> 8) & 0xFF);
    outb64(ATA_PRIMARY_LBA_HI, (lba >> 16) & 0xFF);
    
    // Send read command
    outb64(ATA_PRIMARY_COMMAND, 0x20);  // READ SECTORS
    
    ata_wait_drq64();
    
    // Read 256 words (512 bytes)
    for (int i = 0; i < 256; i++) {
        ((uint16_t*)buffer)[i] = inw64(ATA_PRIMARY_DATA);
    }
    
    // Check for errors
    uint8_t status = inb64(ATA_PRIMARY_STATUS);
    if (status & 0x01) {  // ERR bit
        return 0;  // Error
    }
    
    return 1;  // Success
}

// Write a sector to disk (LBA28 mode)
int disk_write_sector64(uint32_t lba, const uint8_t* buffer) {
    ata_wait_ready64();
    
    // Select drive and set LBA
    outb64(ATA_PRIMARY_DEVICE, 0xE0 | ((lba >> 24) & 0x0F));
    outb64(ATA_PRIMARY_COUNT, 1);  // Write 1 sector
    outb64(ATA_PRIMARY_LBA_LOW, lba & 0xFF);
    outb64(ATA_PRIMARY_LBA_MID, (lba >> 8) & 0xFF);
    outb64(ATA_PRIMARY_LBA_HI, (lba >> 16) & 0xFF);
    
    // Send write command
    outb64(ATA_PRIMARY_COMMAND, 0x30);  // WRITE SECTORS
    
    ata_wait_drq64();
    
    // Write 256 words (512 bytes)
    for (int i = 0; i < 256; i++) {
        outw64(ATA_PRIMARY_DATA, ((uint16_t*)buffer)[i]);
    }
    
    // Flush cache
    outb64(ATA_PRIMARY_COMMAND, 0xE7);  // FLUSH CACHE
    ata_wait_ready64();
    
    // Ekstra bekleme - disk cache'in temizlenmesini garantile
    for (volatile int i = 0; i < 10000; i++);
    
    // Check for errors
    uint8_t status = inb64(ATA_PRIMARY_STATUS);
    if (status & 0x01) {  // ERR bit
        return 0;  // Error
    }
    
    return 1;  // Success
}