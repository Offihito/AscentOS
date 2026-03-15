#ifndef ATA64_H
#define ATA64_H

#include <stdint.h>

// ============================================================
//  AscentOS 64-bit ATA PIO Disk Sürücüsü
//
//  Tüm okuma/yazma LBA48 protokolü ile yapılır.
//  Ext2 sürücüsü (ext2.c) bu fonksiyonları kullanır.
// ============================================================

#define ATA_SECTOR_SIZE  512u

// uint32_t LBA API — Ext2 ve genel kullanım (max ~2 TB)
int disk_read_sector64 (uint32_t lba, uint8_t* buffer);
int disk_write_sector64(uint32_t lba, const uint8_t* buffer);

// uint64_t LBA API — >2 TB diskler için (LBA48 tam aralık)
int disk_read_sector64_ext (uint64_t lba, uint8_t* buffer);
int disk_write_sector64_ext(uint64_t lba, const uint8_t* buffer);

#endif // ATA64_H