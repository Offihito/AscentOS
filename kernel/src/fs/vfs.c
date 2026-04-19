#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"

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

int vfs_truncate(vfs_node_t *node, uint32_t size) {
  if (node && node->truncate) {
    return node->truncate(node, size);
  }
  return -1;
}

int vfs_poll(vfs_node_t *node, int events) {
  if (node && node->poll) {
    return node->poll(node, events);
  }
  // Default: if no poll handler, assume ready for regular files
  uint32_t type = (node->flags & 0xFF);
  if (type == FS_FILE || type == FS_DIRECTORY) {
    return events & (POLLIN | POLLOUT);
  }
  return 0;
}

#define MAX_SYMLINK_DEPTH 8

vfs_node_t *vfs_resolve_path_at(vfs_node_t *dir, const char *path) {
  if (!path || !fs_root)
    return 0;

  char *path_buf = kmalloc(512);
  if (!path_buf)
    return 0;
  strncpy(path_buf, path, 511);
  path_buf[511] = '\0';

  vfs_node_t *current = (path[0] == '/') ? fs_root : (dir ? dir : fs_root);
  int symlink_depth = 0;
  char *p = path_buf;

  // Initial skip of root slashes
  if (path_buf[0] == '/') {
    while (*p == '/')
      p++;
  }

  while (*p) {
    char comp[128];
    int i = 0;

    // Skip slashes
    while (*p == '/')
      p++;
    if (*p == '\0')
      break;

    // Extract next component
    while (*p && *p != '/' && i < 127) {
      comp[i++] = *p++;
    }
    comp[i] = '\0';

    vfs_node_t *next = vfs_finddir(current, comp);
    if (!next) {
      if (current != fs_root && current != dir)
        kfree(current);
      kfree(path_buf);
      return 0;
    }

    // Handle symlinks
    if ((next->flags & 0x7) == FS_SYMLINK) {
      if (++symlink_depth > MAX_SYMLINK_DEPTH) {
        kfree(next);
        if (current != fs_root && current != dir)
          kfree(current);
        kfree(path_buf);
        return 0;
      }

      char link_target[256];
      int len = vfs_readlink(next, link_target, 256);
      kfree(next);

      if (len < 0) {
        if (current != fs_root && current != dir)
          kfree(current);
        kfree(path_buf);
        return 0;
      }

      // Construct new path: [link_target] + "/" + [remaining p]
      char *next_path = kmalloc(512);
      if (!next_path) {
        if (current != fs_root && current != dir)
          kfree(current);
        kfree(path_buf);
        return 0;
      }
      strncpy(next_path, link_target, 511);
      next_path[511] = '\0';

      if (*p) {
        int cur_len = strlen(next_path);
        if (cur_len < 510) {
          if (next_path[cur_len - 1] != '/') {
            strcat(next_path, "/");
          }
          strncat(next_path, p, 511 - strlen(next_path));
        }
      }
      next_path[511] = '\0';

      // Update path_buf and p
      strcpy(path_buf, next_path);
      kfree(next_path);
      p = path_buf;

      if (path_buf[0] == '/') {
        if (current != fs_root && current != dir)
          kfree(current);
        current = fs_root;
        while (*p == '/')
          p++;
      }
      // Continue loop with new path and same current (if relative) or root (if
      // absolute)
      continue;
    }

    // Move to next directory component
    if (current != fs_root && current != dir) {
      kfree(current);
    }
    current = next;
  }

  kfree(path_buf);
  return current;
}

vfs_node_t *vfs_resolve_path(const char *path) {
  return vfs_resolve_path_at(fs_root, path);
}
