#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>

// ============================================================
//  AscentOS — Ext2 Filesystem Driver
//  disk_read_sector64 / disk_write_sector64 üzerinden çalışır.
//  Block size: 1024 byte (LBA2'den itibaren)
// ============================================================

// ----------------------------------------------------------
//  dirent64_t  (files64.h ile paylaşılır — include guard korumalı)
// ----------------------------------------------------------
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

#endif /* DIRENT64_T_DEFINED */

// ----------------------------------------------------------
//  Ext2 sabitler
// ----------------------------------------------------------
#define EXT2_SUPER_MAGIC        0xEF53
#define EXT2_SUPERBLOCK_OFFSET  1024u      // byte cinsinden disk başından
#define EXT2_SUPERBLOCK_LBA     2u         // 512-byte sektör numarası
#define EXT2_ROOT_INO           2u
#define EXT2_BAD_INO            1u
#define EXT2_LOST_FOUND_INO     11u

// Inode tip bitleri (i_mode yüksek bitleri)
#define EXT2_S_IFREG    0x8000u    // düzenli dosya
#define EXT2_S_IFDIR    0x4000u    // dizin
#define EXT2_S_IFLNK    0xA000u    // sembolik link
#define EXT2_S_IFMT     0xF000u    // tip maskesi

// Dizin entry dosya tipleri
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_SYMLINK  7

// Boyutlar
#define EXT2_SECTOR_SIZE    512u
#define EXT2_MAX_PATH       256u
#define EXT2_MAX_NAME       255u
#define EXT2_INODE_SIZE_REV0 128u

// ----------------------------------------------------------
//  On-disk yapılar (little-endian, packed)
// ----------------------------------------------------------

// Superblock — 1024 byte, LBA 2'de (byte offset 1024)
typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;    // küçük fs: 1, 1024B block; büyük fs: 0
    uint32_t s_log_block_size;      // block_size = 1024 << s_log_block_size
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;               // 0xEF53
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;           // 0=old, 1=dynamic
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    // EXT2_DYNAMIC_REV (rev_level >= 1) ek alanlar
    uint32_t s_first_ino;           // ilk kullanılabilir inode
    uint16_t s_inode_size;          // inode yapısı boyutu
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    uint8_t  _pad[820];             // toplam 1024 byte
} Ext2Superblock;

// Block Group Descriptor — 32 byte
typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;       // block bitmap'in block numarası
    uint32_t bg_inode_bitmap;       // inode bitmap'in block numarası
    uint32_t bg_inode_table;        // inode tablosunun ilk block numarası
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} Ext2GroupDesc;

// Inode — 128 byte (rev0)
typedef struct __attribute__((packed)) {
    uint16_t i_mode;                // dosya tipi + izin bitleri
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;              // 512-byte biriminde disk blok sayısı
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];           // [0..11]=direct, [12]=indirect,
                                    // [13]=dindirect, [14]=tindirect
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} Ext2Inode;

// Dizin entry — değişken boyutlu
typedef struct __attribute__((packed)) {
    uint32_t inode;                 // 0 = boş entry
    uint16_t rec_len;               // bu entry'nin toplam byte boyutu
    uint8_t  name_len;
    uint8_t  file_type;             // EXT2_FT_*
    char     name[EXT2_MAX_NAME];   // null-terminated DEĞİL; name_len byte geçerli
} Ext2DirEntry;

// ----------------------------------------------------------
//  Mount state (runtime)
// ----------------------------------------------------------
typedef struct {
    int      mounted;
    uint32_t block_size;            // byte cinsinden (1024, 2048, 4096)
    uint32_t sectors_per_block;     // block_size / 512
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t inode_size;            // byte cinsinden
    uint32_t first_data_block;      // s_first_data_block
    uint32_t num_groups;
    uint32_t first_ino;             // ilk kullanılabilir inode no
    uint32_t inodes_count;          // toplam inode sayısı
    uint32_t blocks_count;          // toplam block sayısı
    char     cwd[EXT2_MAX_PATH];    // current working directory
} Ext2State;

// ----------------------------------------------------------
//  Public API
// ----------------------------------------------------------

// Mount / unmount / format
int  ext2_mount(void);
int  ext2_unmount(void);
int  ext2_format(void);  // Boş diske minimal ext2 yazar; mount başarısız olunca çağrılır

// Okuma işlemleri
int      ext2_read_file   (const char* path, uint8_t* buf, uint32_t max_len);
uint32_t ext2_file_size   (const char* path);
int      ext2_path_is_file(const char* path);
int      ext2_path_is_dir (const char* path);
int      ext2_getdents    (const char* path, dirent64_t* buf, int buf_size);

// Yazma işlemleri
int ext2_write_file  (const char* path, uint64_t offset,
                      const uint8_t* data, uint32_t len);
int ext2_create_file (const char* path);
int ext2_mkdir       (const char* path);
int ext2_mkdir_p     (const char* path);  // recursive — eksik parent'ları da oluşturur
int ext2_rmdir       (const char* path);
int ext2_unlink      (const char* path);
int ext2_rename      (const char* oldpath, const char* newpath);
int ext2_truncate    (const char* path, uint64_t length);

// CWD
const char* ext2_getcwd(void);
int         ext2_chdir (const char* path);

// Dahili state (tanı / test için)
const Ext2State* ext2_get_state(void);

#endif // EXT2_H