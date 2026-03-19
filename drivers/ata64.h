#ifndef ATA64_H
#define ATA64_H

#include <stdint.h>

#define ATA_SECTOR_SIZE  512u

int disk_read_sector64 (uint32_t lba, uint8_t* buffer);
int disk_write_sector64(uint32_t lba, const uint8_t* buffer);

int disk_read_sector64_ext (uint64_t lba, uint8_t* buffer);
int disk_write_sector64_ext(uint64_t lba, const uint8_t* buffer);

#endif