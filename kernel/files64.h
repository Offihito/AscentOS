#ifndef FILES64_H
#define FILES64_H

#include <stdint.h>

#ifndef MAX_FILES
#define MAX_FILES 32
#endif

#ifndef MAX_DIRS
#define MAX_DIRS 16
#endif

#ifndef MAX_PATH_LENGTH
#define MAX_PATH_LENGTH 128
#endif

// File structure
typedef struct {
    const char* name;
    const char* content;
    uint32_t size;
    uint8_t is_dynamic;
    const char* directory;  // Parent directory path
} EmbeddedFile64;

// Directory structure
typedef struct {
    char path[MAX_PATH_LENGTH];
    uint8_t is_dynamic;
} Directory64;

// File system function declarations
void init_filesystem64(void);
int fs_list_files64(void* output_ptr);
const EmbeddedFile64* fs_get_file64(const char* filename);
int fs_touch_file64(const char* filename);
int fs_write_file64(const char* filename, const char* content);
int fs_delete_file64(const char* filename);
void save_files_to_disk64(void);
const EmbeddedFile64* get_all_files_list64(int* count);

// Directory operations
int fs_mkdir64(const char* dirname);
int fs_rmdir64(const char* dirname);
int fs_chdir64(const char* dirname);
const char* fs_getcwd64(void);
int fs_list_dirs64(void* output_ptr);

#endif // FILES64_H