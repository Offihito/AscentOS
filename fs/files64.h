#ifndef FILES64_H
#define FILES64_H

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

// ----------------------------------------------------------
//  Maximum content size for a single dynamic file stored in
//  the in-memory VFS.  Previously this was hard-capped at 512
//  bytes (one ATA sector); now we use 1 MiB because persistence
//  is handled by FAT32 which supports arbitrarily large files.
//  Raise this constant if you need larger in-memory files.
// ----------------------------------------------------------
#ifndef MAX_FILE_SIZE
#define MAX_FILE_SIZE (1u * 1024u * 1024u)   /* 1 MiB */
#endif

// File structure
typedef struct {
    const char* name;
    const char* content;
    uint32_t    size;
    uint8_t     is_dynamic;
    const char* directory;   // Parent directory path
} EmbeddedFile64;

// Directory structure with metadata
typedef struct {
    char     path[MAX_PATH_LENGTH];
    uint8_t  is_dynamic;
    uint8_t  is_system;       // System directory (can't delete)
    uint8_t  permissions;     // Basic permission bits
    uint32_t created_time;    // Creation timestamp
} Directory64;

// ---- File system function declarations ----
void init_filesystem64(void);
int  fs_list_files64(void* output_ptr);
const EmbeddedFile64* fs_get_file64(const char* filename);
int  fs_touch_file64(const char* filename);
int  fs_write_file64(const char* filename, const char* content);
int  fs_delete_file64(const char* filename);
void save_files_to_disk64(void);
const EmbeddedFile64* get_all_files_list64(int* count);

// ---- Directory operations ----
int         fs_mkdir64(const char* dirname);
int         fs_rmdir64(const char* dirname);
int         fs_rmdir_recursive64(const char* dirname);
int         fs_chdir64(const char* dirname);
const char* fs_getcwd64(void);
int         fs_list_dirs64(void* output_ptr);

// ---- Advanced directory operations ----
int fs_tree64(void* output_ptr);
int fs_find64(const char* pattern, void* output_ptr);
int fs_du64(const char* path, void* output_ptr);
int fs_count_subdirs(const char* path);
int fs_count_files_in_tree(const char* path);

// ---- Syscall yardımcıları (SYS_STAT / SYS_ACCESS için) ----
// Verilen path bir dosya mı? 1=evet, 0=hayır
int      fs_path_is_file(const char* path);
// Verilen path bir dizin mi? 1=evet, 0=hayır
int      fs_path_is_dir(const char* path);
// Path'in dosya boyutunu döndür (dosya yoksa 0)
uint32_t fs_path_filesize(const char* path);

// ---- Syscall yazma işlemleri (SYS_UNLINK / SYS_RENAME için) (v14) ----
// Dosyayı siler (dizinse -1 döner)
int fs_unlink64(const char* path);
// Dosya veya dizini yeniden adlandırır/taşır
int fs_rename64(const char* oldpath, const char* newpath);

// ---- SYS_GETDENTS için (v9) ----
// d_type sabitleri
#define DT_UNKNOWN  0
#define DT_REG      8   // regular file
#define DT_DIR      4   // directory

// Linux getdents64 dirent yapısı
typedef struct {
    uint64_t d_ino;      // inode numarası (stub)
    uint64_t d_off;      // bir sonraki entry'nin ofseti
    uint16_t d_reclen;   // bu struct'ın toplam boyutu (hizalanmış)
    uint8_t  d_type;     // DT_REG | DT_DIR | DT_UNKNOWN
    char     d_name[256]; // null-terminated dosya/dizin adı
} __attribute__((packed)) dirent64_t;

// path altındaki entry'leri buf'a yazar.
// Döndürür: yazılan toplam byte sayısı | -1 (hata)
int fs_getdents64(const char* path, dirent64_t* buf, int buf_size);

#endif // FILES64_H