#ifndef EXT3_H
#define EXT3_H

#include <stdint.h>
#include "journal.h"

// ============================================================
//  AscentOS — Ext3 Filesystem Driver
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
#define EXT3_SUPER_MAGIC        0xEF53
#define EXT3_SUPERBLOCK_OFFSET  1024u
#define EXT3_SUPERBLOCK_LBA     2u
#define EXT3_ROOT_INO           2u
#define EXT3_BAD_INO            1u
#define EXT3_LOST_FOUND_INO     11u

// Inode tip bitleri (i_mode yüksek bitleri)
#define EXT3_S_IFREG    0x8000u
#define EXT3_S_IFDIR    0x4000u
#define EXT3_S_IFLNK    0xA000u
#define EXT3_S_IFMT     0xF000u

// Filesystem state (s_state)
#define EXT3_VALID_FS   1u
#define EXT3_ERROR_FS   2u

// EXT3 Journal inode
#define EXT3_JOURNAL_INO    8u

// Dizin entry dosya tipleri
#define EXT3_FT_UNKNOWN  0
#define EXT3_FT_REG_FILE 1
#define EXT3_FT_DIR      2
#define EXT3_FT_SYMLINK  7

// Boyutlar
#define EXT3_SECTOR_SIZE    512u
#define EXT3_MAX_PATH       256u
#define EXT3_MAX_NAME       255u
#define EXT3_INODE_SIZE_REV0 128u

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
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    // EXT2_DYNAMIC_REV (rev_level >= 1) ek alanlar
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    // EXT3 Journal fields
    uint32_t s_journal_ino;         // EXT3: Journal inode number
    uint32_t s_journal_dev;         // EXT3: External journal device
    uint32_t s_last_orphan;         // EXT3: Last orphan inode
    uint8_t  _pad[808];             // Remaining padding (820 - 12 = 808)
} Ext3Superblock;

// EXT3 Feature Flags (Superblock'taki s_feature_* alanları yönetmek için)
#define EXT3_FEATURE_COMPAT_DIR_PREALLOC    0x0001u
#define EXT3_FEATURE_COMPAT_IMAGIC_INODES   0x0002u
#define EXT3_FEATURE_COMPAT_HAS_JOURNAL     0x0004u

#define EXT3_FEATURE_INCOMPAT_COMPRESSION   0x0001u
#define EXT3_FEATURE_INCOMPAT_FILETYPE      0x0002u
#define EXT3_FEATURE_INCOMPAT_RECOVER       0x0004u
#define EXT3_FEATURE_INCOMPAT_JOURNAL_DEV   0x0008u

#define EXT3_FEATURE_RO_COMPAT_SPARSE_SUPER  0x0001u
#define EXT3_FEATURE_RO_COMPAT_LARGE_FILE    0x0002u
#define EXT3_FEATURE_RO_COMPAT_BTREE_DIR     0x0004u

// Block Group Descriptor — 32 byte
typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} Ext3GroupDesc;

// Inode — 128 byte (rev0)
typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} Ext3Inode;

// Dizin entry — değişken boyutlu
typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[EXT3_MAX_NAME];
} Ext3DirEntry;

// ----------------------------------------------------------
//  Mount state (runtime)
// ----------------------------------------------------------
typedef struct {
    int      mounted;
    uint32_t block_size;
    uint32_t sectors_per_block;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t inode_size;
    uint32_t first_data_block;
    uint32_t num_groups;
    uint32_t first_ino;
    uint32_t inodes_count;
    uint32_t blocks_count;
    char     cwd[EXT3_MAX_PATH];
    
    // EXT3 Journal Desteği
    int      journal_enabled;
    uint32_t journal_ino;
    JournalHandle* current_handle;
    volatile int journal_lock;
} Ext3State;

// ----------------------------------------------------------
//  Public API — Basic Operations
// ----------------------------------------------------------

// Mount / unmount / format
int  ext3_mount(void);
int  ext3_unmount(void);
int  ext3_format(void);

// Read operations
int      ext3_read_file   (const char* path, uint8_t* buf, uint32_t max_len);
int      ext3_read_file_at(const char* path, uint32_t offset,
                           uint8_t* buf, uint32_t max_len);
uint32_t ext3_file_size   (const char* path);
int      ext3_path_is_file(const char* path);
int      ext3_path_is_dir (const char* path);
int      ext3_getdents    (const char* path, dirent64_t* buf, int buf_size);

// Write operations
int ext3_write_file  (const char* path, uint64_t offset,
                      const uint8_t* data, uint32_t len);
int ext3_create_file (const char* path);
int ext3_mkdir       (const char* path);
int ext3_mkdir_p     (const char* path);
int ext3_rmdir       (const char* path);
int ext3_unlink      (const char* path);
int ext3_rename      (const char* oldpath, const char* newpath);
int ext3_truncate    (const char* path, uint64_t length);

// Path and directory operations
const char* ext3_getcwd(void);
int         ext3_chdir (const char* path);
uint32_t    ext3_path_to_ino(const char* path);
int         ext3_read_inode_at(uint32_t ino, uint32_t offset,
                               uint8_t* buf, uint32_t max_len);

// State query
const Ext3State* ext3_get_state(void);

// ----------------------------------------------------------
//  Public API — EXT3 Journaling
// ----------------------------------------------------------

// Journal initialization
int ext3_init_journal(void);
int ext3_check_recovery_needed(void);

// Transaction-aware operations
int ext3_write_file_transacted(const char* path, uint64_t offset,
                               const uint8_t* data, uint32_t len);
int ext3_create_file_transacted(const char* path);
int ext3_mkdir_transacted(const char* path);
int ext3_unlink_transacted(const char* path);
int ext3_truncate_transacted(const char* path, uint64_t length);

// Block operations with journal
int ext3_alloc_block_transacted(JournalHandle* h, uint32_t* out_block);
int ext3_free_block_transacted(JournalHandle* h, uint32_t block);

// Inode operations with journal
int ext3_get_write_access_inode(JournalHandle* h, uint32_t ino);

// Orphan inode management
int ext3_add_to_orphan_list(JournalHandle* h, uint32_t ino);
int ext3_remove_from_orphan_list(JournalHandle* h, uint32_t ino);

#endif // EXT3_H