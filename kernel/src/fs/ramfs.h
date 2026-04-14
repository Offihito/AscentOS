#ifndef FS_RAMFS_H
#define FS_RAMFS_H

#include "fs/vfs.h"

// Initialize the root ramfs and mount it to fs_root
void ramfs_init(void);

// Adds a pre-existing vfs_node_t to the root ramfs directory directly
// (Useful for mounting block devices into /dev early on)
void ramfs_mount_node(vfs_node_t *root, vfs_node_t *node);

// Exposed ramfs read/write for kernel-internal pipe buffers
uint32_t ramfs_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
uint32_t ramfs_write(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);

#endif
