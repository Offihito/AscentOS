#ifndef FS_RAMFS_H
#define FS_RAMFS_H

#include "fs/vfs.h"

// Initialize the root ramfs and mount it to fs_root
void ramfs_init(void);

// Adds a pre-existing vfs_node_t to the root ramfs directory directly
// (Useful for mounting block devices into /dev early on)
void ramfs_mount_node(vfs_node_t *root, vfs_node_t *node);

#endif
