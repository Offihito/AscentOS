#ifndef DISK64_H
#define DISK64_H

#include <stdint.h>

// Disk I/O function declarations
int disk_read_sector64(uint32_t lba, uint8_t* buffer);
int disk_write_sector64(uint32_t lba, const uint8_t* buffer);

#endif // DISK64_H