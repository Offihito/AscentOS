#ifndef DISK64_H
#define DISK64_H

#include <stdint.h>

// ============================================================
//  FAT32 Disk Driver for AscentOS 64-bit
//  Replaces raw LBA sector I/O with a full FAT32 layer.
//  The kernel stores its persistent filesystem inside a
//  dedicated FAT32 partition that starts at a fixed LBA.
// ============================================================

// ----------------------------------------------------------
// Tuneable constants
// ----------------------------------------------------------

// First LBA sector of the FAT32 partition on the disk.
// Change this to match your actual partition layout.
#define FAT32_PARTITION_LBA     2048u

// Maximum file size supported by the VFS layer
// FAT32 theoretical max: 4 GB - 1 byte (0xFFFFFFFF)
// Practical limit for our kernel: 256 MB (plenty for OS files)
#define FAT32_MAX_FILE_BYTES    (256u * 1024u * 1024u)  // 256 MB

// ----------------------------------------------------------
// FAT32 on-disk structures  (all little-endian)
// ----------------------------------------------------------

#define FAT32_SECTOR_SIZE   512u
#define FAT32_EOC           0x0FFFFFF8u   // End-of-chain marker
#define FAT32_FREE_CLUSTER  0x00000000u
#define FAT32_BAD_CLUSTER   0x0FFFFFF7u
#define FAT32_DIR_ENTRIES_PER_SECTOR  (FAT32_SECTOR_SIZE / 32u)

// BIOS Parameter Block (BPB) – first sector of the partition
typedef struct __attribute__((packed)) {
    uint8_t  jmp_boot[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;       // 0 for FAT32
    uint16_t total_sectors_16;       // 0 for FAT32
    uint8_t  media_type;
    uint16_t fat_size_16;            // 0 for FAT32
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    // FAT32 extended BPB
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;          // Usually cluster 2
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];            // "FAT32   "
} Fat32BPB;

// Standard 32-byte directory entry
typedef struct __attribute__((packed)) {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attr;
    uint8_t  nt_res;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;           // High 16 bits of first cluster
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;           // Low 16 bits of first cluster
    uint32_t file_size;
} Fat32DirEntry;

// Directory entry attribute flags
#define FAT32_ATTR_READ_ONLY    0x01
#define FAT32_ATTR_HIDDEN       0x02
#define FAT32_ATTR_SYSTEM       0x04
#define FAT32_ATTR_VOLUME_ID    0x08
#define FAT32_ATTR_DIRECTORY    0x10
#define FAT32_ATTR_ARCHIVE      0x20
#define FAT32_ATTR_LFN          0x0F    // Long filename entry

// ----------------------------------------------------------
// FAT32 runtime state (cached geometry)
// ----------------------------------------------------------
typedef struct {
    uint32_t partition_lba;         // Absolute LBA of BPB sector
    uint32_t fat_lba;               // LBA of first FAT
    uint32_t fat2_lba;              // LBA of second FAT (mirror)
    uint32_t data_lba;              // LBA of cluster-2 data region
    uint32_t root_cluster;          // First cluster of root directory
    uint32_t sectors_per_cluster;
    uint32_t fat_size_sectors;
    uint32_t total_clusters;
    uint8_t  mounted;               // 1 once fat32_mount() succeeds
} Fat32State;

// ----------------------------------------------------------
// Public API
// ----------------------------------------------------------

// Low-level ATA helpers (used internally and exported for
// callers that still need raw sector access, e.g. pmm init).
int  disk_read_sector64 (uint32_t lba, uint8_t* buffer);
int  disk_write_sector64(uint32_t lba, const uint8_t* buffer);

// FAT32 mount / format
// fat32_mount()  – tries to read BPB from FAT32_PARTITION_LBA.
//                  Returns 1 on success, 0 if no valid FAT32 found.
// fat32_format() – writes a minimal FAT32 BPB + FATs + root dir
//                  so a fresh disk can be used immediately.
int fat32_mount (void);
int fat32_format(void);

// File operations
// All paths are 8.3 upper-case names relative to root for now.
// Returns 1 on success, 0 on failure.
int      fat32_create_file(const char* name83);
int      fat32_delete_file(const char* name83);
// Read file into caller-supplied buffer (max_len bytes).
// Returns actual bytes read, or -1 on error.
int      fat32_read_file  (const char* name83, uint8_t* buf, uint32_t max_len);
// Write / overwrite file. Returns 1 on success.
int      fat32_write_file (const char* name83, const uint8_t* buf, uint32_t len);
// Returns file size in bytes, or 0 if not found.
uint32_t fat32_file_size  (const char* name83);

// Directory entry iteration (root directory only for now)
// Fills *entry and returns 1 while entries remain; returns 0 when done.
// Pass *index = 0 to start; the function advances *index internally.
int fat32_next_entry(uint32_t* index, Fat32DirEntry* entry);

// Expose internal state (read-only) for diagnostics / formatting
const Fat32State* fat32_get_state(void);

#endif // DISK64_H