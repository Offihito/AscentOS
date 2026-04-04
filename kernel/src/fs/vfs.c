#include "fs/vfs.h"

vfs_node_t *fs_root = 0;

uint32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (node && node->read) {
        return node->read(node, offset, size, buffer);
    }
    return 0;
}

uint32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (node && node->write) {
        return node->write(node, offset, size, buffer);
    }
    return 0;
}

void vfs_open(vfs_node_t *node) {
    if (node && node->open) {
        node->open(node);
    }
}

void vfs_close(vfs_node_t *node) {
    if (node && node->close) {
        node->close(node);
    }
}

struct dirent *vfs_readdir(vfs_node_t *node, uint32_t index) {
    if ((node->flags & 0x07) == FS_DIRECTORY && node->readdir) {
        return node->readdir(node, index);
    }
    return 0;
}

vfs_node_t *vfs_finddir(vfs_node_t *node, char *name) {
    if ((node->flags & 0x07) == FS_DIRECTORY && node->finddir) {
        return node->finddir(node, name);
    }
    return 0;
}

int vfs_create(vfs_node_t *node, char *name, uint16_t permission) {
    if ((node->flags & 0x07) == FS_DIRECTORY && node->create) {
        return node->create(node, name, permission);
    }
    return -1;
}

int vfs_mkdir(vfs_node_t *node, char *name, uint16_t permission) {
    if ((node->flags & 0x07) == FS_DIRECTORY && node->mkdir) {
        return node->mkdir(node, name, permission);
    }
    return -1;
}
