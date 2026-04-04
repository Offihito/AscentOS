#include "drivers/storage/block.h"
#include <stddef.h>

static struct block_device *registered[BLOCK_MAX_DEVICES];
static int num_devices = 0;

#include "fs/vfs.h"
#include "fs/ramfs.h"
#include "mm/heap.h"
#include "lib/string.h"

static uint32_t block_vfs_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    struct block_device *dev = (struct block_device *)node->device;
    if (!dev || !dev->read_sectors) return 0;
    
    uint32_t sector_size = dev->sector_size ? dev->sector_size : 512;
    uint32_t sector = offset / sector_size;
    uint32_t count = size / sector_size;
    
    if (size % sector_size != 0) return 0; // Enforce sector aligned logical reads for now
    
    int err = dev->read_sectors(dev, sector, count, buffer);
    if (err) return 0;
    return size;
}

int block_register(struct block_device *dev) {
    if (num_devices >= BLOCK_MAX_DEVICES) return -1;
    registered[num_devices++] = dev;
    
    if (fs_root) {
        vfs_node_t *dev_dir = vfs_finddir(fs_root, "dev");
        if (dev_dir) {
            vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
            memset(node, 0, sizeof(vfs_node_t));
            strncpy(node->name, dev->name, 127);
            node->flags = FS_BLOCKDEV;
            node->mask = 0600;
            node->length = dev->total_sectors * (dev->sector_size ? dev->sector_size : 512);
            node->device = dev;
            node->read = block_vfs_read;
            ramfs_mount_node(dev_dir, node);
        }
    }
    
    return 0;
}

struct block_device *block_get(int index) {
    if (index < 0 || index >= num_devices) return NULL;
    return registered[index];
}

int block_count(void) {
    return num_devices;
}
