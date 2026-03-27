#ifndef FILES64_H
#define FILES64_H

#include "vfs.h"
#include <stdint.h>

#ifndef MAX_FILES
#define MAX_FILES 2048
#endif

#ifndef MAX_DIRS
#define MAX_DIRS 512
#endif

#ifndef MAX_PATH_LENGTH
#define MAX_PATH_LENGTH 256
#endif

#ifndef MAX_FILE_SIZE
#define MAX_FILE_SIZE (1u * 1024u * 1024u)  /* 1 MiB per file */
#endif

// File structure
// typedef struct {
//     const char* name;
//     const char* content;
//     uint32_t    size;
//     uint8_t     is_dynamic;
//     const char* directory;
// } EmbeddedFile64;

// Directory structure with metadata
typedef struct {
    char     path[MAX_PATH_LENGTH];
    uint8_t  is_dynamic;
    uint8_t  is_system;
    uint16_t permissions;
    uint32_t created_time;
} Directory64;

void init_filesystem64(void);
int  fs_list_files64(void* output_ptr);
// const EmbeddedFile64* fs_get_file64(const char* filename);
// int  fs_touch_file64(const char* filename);
// int  fs_write_file64(const char* filename, const char* content);
// int  fs_delete_file64(const char* filename);
void save_files_to_disk64(void);
const EmbeddedFile64* get_all_files_list64(int* count);

// Directory operations
int         fs_mkdir64(const char* dirname);
int         fs_rmdir64(const char* dirname);
int         fs_rmdir_recursive64(const char* dirname);
int         fs_chdir64(const char* dirname);
const char* fs_getcwd64(void);
int         fs_list_dirs64(void* output_ptr);

// Advanced operations
int fs_tree64(void* output_ptr);
int fs_find64(const char* pattern, void* output_ptr);
int fs_du64(const char* path, void* output_ptr);
int fs_count_subdirs(const char* path);
int fs_count_files_in_tree(const char* path);

// Path helpers
int      fs_path_is_file(const char* path);
int      fs_path_is_dir(const char* path);
uint32_t fs_path_filesize(const char* path);

// Syscall-level operations
int fs_unlink64(const char* path);
int fs_rename64(const char* oldpath, const char* newpath);
int fs_truncate64(const char* path, uint64_t length);

// Internal helpers
char* pool_alloc(uint32_t size);
extern EmbeddedFile64 all_files64[];
extern char* dynamic_content_ptr[];
extern char current_dir[];
extern Directory64* directories;
extern int dir_count;
extern int file_count64;
extern char dynamic_names64[][64];
extern char dynamic_dirs64[][256];
extern char content_pool[];
extern uint32_t pool_offset;

// String helpers
int str_starts_with(const char* str, const char* prefix);
int str_contains(const char* str, const char* substr);

// Path helpers
void normalize_path(const char* input, char* output);
int dir_exists(const char* path);
void get_parent_dir(const char* path, char* parent);
void get_dir_name(const char* path, char* name);

#ifndef DIRENT64_T_DEFINED
#define DIRENT64_T_DEFINED

#define DT_UNKNOWN  0
#define DT_REG      8
#define DT_DIR      4

typedef struct {
    uint64_t d_ino;
    uint64_t d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
} __attribute__((packed)) dirent64_t;

#endif

int fs_getdents64(const char* path, dirent64_t* buf, int buf_size);

// ---- sys_write bridge ----
// int fs_vfs_write(const char* path, uint64_t offset,
//                  const char* data, uint32_t len);

#endif