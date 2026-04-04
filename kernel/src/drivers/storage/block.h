#ifndef BLOCK_BLOCK_H
#define BLOCK_BLOCK_H

#include <stdint.h>

#define BLOCK_MAX_DEVICES 4
#define BLOCK_SECTOR_SIZE 512

struct block_device {
    char name[16];              // e.g. "ata0", "ata1"
    uint32_t sector_size;       // Usually 512
    uint64_t total_sectors;     // Total number of sectors on the device
    int (*read_sectors)(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write_sectors)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
    void *driver_data;          // Opaque pointer for the specific driver
};

// Register a block device. Returns 0 on success, -1 if full.
int block_register(struct block_device *dev);

// Get a registered block device by index. Returns NULL if invalid.
struct block_device *block_get(int index);

// Get the number of registered block devices.
int block_count(void);

#endif
