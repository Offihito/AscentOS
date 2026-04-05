#include "fs/vfs.h"

vfs_node_t *fs_root = 0;

uint32_t vfs_read(vfs_node_t *node, uint32_t offset, uint32_t size,
                  uint8_t *buffer) {
  if (node && node->read) {
    return node->read(node, offset, size, buffer);
  }
  return 0;
}

uint32_t vfs_write(vfs_node_t *node, uint32_t offset, uint32_t size,
                   uint8_t *buffer) {
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

int vfs_unlink(vfs_node_t *node, char *name) {
  if ((node->flags & 0x07) == FS_DIRECTORY && node->unlink) {
    return node->unlink(node, name);
  }
  return -1;
}

int vfs_rmdir(vfs_node_t *node, char *name) {
  if ((node->flags & 0x07) == FS_DIRECTORY && node->rmdir) {
    return node->rmdir(node, name);
  }
  return -1;
}

int vfs_readlink(vfs_node_t *node, char *buf, uint32_t size) {
  if (node && node->readlink) {
    return node->readlink(node, buf, size);
  }
  return -1;
}

int vfs_symlink(vfs_node_t *node, char *name, char *target) {
  if ((node->flags & 0x07) == FS_DIRECTORY && node->symlink) {
    return node->symlink(node, name, target);
  }
  return -1;
}

int vfs_rename(vfs_node_t *node, char *old_name, char *new_name) {
  if ((node->flags & 0x07) == FS_DIRECTORY && node->rename) {
    return node->rename(node, old_name, new_name);
  }
  return -1;
}

int vfs_chmod(vfs_node_t *node, uint16_t permission) {
  if (node && node->chmod) {
    return node->chmod(node, permission);
  }
  return -1;
}

int vfs_chown(vfs_node_t *node, uint32_t uid, uint32_t gid) {
  if (node && node->chown) {
    return node->chown(node, uid, gid);
  }
  return -1;
}
