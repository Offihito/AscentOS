#ifndef VFS_H
#define VFS_H

#include <stdint.h>

// File structure
typedef struct {
    const char* name;
    const char* content;
    uint32_t    size;
    uint8_t     is_dynamic;
    const char* directory;
} EmbeddedFile64;

// ---- VFS operations ----
const EmbeddedFile64* fs_get_file64(const char* filename);
int fs_touch_file64(const char* filename);
int fs_write_file64(const char* name, const char* content);
int fs_delete_file64(const char* name);

// ---- sys_write bridge ----
int fs_vfs_write(const char* path, uint64_t offset,
                 const char* data, uint32_t len);

#endif