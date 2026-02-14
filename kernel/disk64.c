// disk64.c - FAT32 with optimized geometry for larger volumes
// Changes from previous version:
// - Increased disk size: 100MB -> 2GB
// - Larger clusters: 1 sector -> 8 sectors (4KB)
// - More efficient for larger files
// - Still supports up to 256MB files

#include "disk64.h"
#include <stdint.h>
#include <stddef.h>

// ============================================================
//  ATA PIO port map (primary channel, master drive)
// ============================================================
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECTOR_CNT  0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DEVICE      0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7

#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30
#define ATA_CMD_FLUSH   0xE7

#define ATA_SR_BSY      0x80
#define ATA_SR_DRQ      0x08
#define ATA_SR_ERR      0x01

// ============================================================
//  I/O helpers
// ============================================================
static inline uint8_t inb_p(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outb_p(uint16_t port, uint8_t v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint16_t inw_p(uint16_t port) {
    uint16_t v;
    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw_p(uint16_t port, uint16_t v) {
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}

static void ata_wait_bsy(void) {
    while (inb_p(ATA_STATUS) & ATA_SR_BSY);
}
static void ata_wait_drq(void) {
    while (!(inb_p(ATA_STATUS) & ATA_SR_DRQ));
}

// ============================================================
//  Public: raw sector read / write  (LBA28)
// ============================================================
int disk_read_sector64(uint32_t lba, uint8_t* buf) {
    ata_wait_bsy();
    outb_p(ATA_DEVICE,    0xE0 | ((lba >> 24) & 0x0F));
    outb_p(ATA_SECTOR_CNT, 1);
    outb_p(ATA_LBA_LO,    (uint8_t)(lba));
    outb_p(ATA_LBA_MID,   (uint8_t)(lba >> 8));
    outb_p(ATA_LBA_HI,    (uint8_t)(lba >> 16));
    outb_p(ATA_COMMAND,   ATA_CMD_READ);
    ata_wait_drq();
    for (int i = 0; i < 256; i++)
        ((uint16_t*)buf)[i] = inw_p(ATA_DATA);
    if (inb_p(ATA_STATUS) & ATA_SR_ERR) return 0;
    return 1;
}

int disk_write_sector64(uint32_t lba, const uint8_t* buf) {
    ata_wait_bsy();
    outb_p(ATA_DEVICE,    0xE0 | ((lba >> 24) & 0x0F));
    outb_p(ATA_SECTOR_CNT, 1);
    outb_p(ATA_LBA_LO,    (uint8_t)(lba));
    outb_p(ATA_LBA_MID,   (uint8_t)(lba >> 8));
    outb_p(ATA_LBA_HI,    (uint8_t)(lba >> 16));
    outb_p(ATA_COMMAND,   ATA_CMD_WRITE);
    ata_wait_drq();
    for (int i = 0; i < 256; i++)
        outw_p(ATA_DATA, ((const uint16_t*)buf)[i]);
    outb_p(ATA_COMMAND, ATA_CMD_FLUSH);
    ata_wait_bsy();
    for (volatile int i = 0; i < 10000; i++);
    if (inb_p(ATA_STATUS) & ATA_SR_ERR) return 0;
    return 1;
}

// ============================================================
//  Internal: tiny memory helpers
// ============================================================
static void k_memset(void* dst, uint8_t val, uint32_t n) {
    uint8_t* p = (uint8_t*)dst;
    while (n--) *p++ = val;
}
static void k_memcpy(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
}
static int k_memcmp(const void* a, const void* b, uint32_t n) {
    const uint8_t* p = (const uint8_t*)a;
    const uint8_t* q = (const uint8_t*)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

// Little-endian helpers
static uint32_t read_le32(const uint8_t* p) {
    return ((uint32_t)p[0]) | 
           ((uint32_t)p[1] << 8) | 
           ((uint32_t)p[2] << 16) | 
           ((uint32_t)p[3] << 24);
}

static void write_le32(uint8_t* p, uint32_t val) {
    p[0] = (uint8_t)(val);
    p[1] = (uint8_t)(val >> 8);
    p[2] = (uint8_t)(val >> 16);
    p[3] = (uint8_t)(val >> 24);
}

static uint16_t read_le16(const uint8_t* p) {
    return ((uint16_t)p[0]) | ((uint16_t)p[1] << 8);
}

static void write_le16(uint8_t* p, uint16_t val) {
    p[0] = (uint8_t)(val);
    p[1] = (uint8_t)(val >> 8);
}

// ============================================================
//  FAT32 runtime state
// ============================================================
static Fat32State fat32 = {0};

const Fat32State* fat32_get_state(void) { return &fat32; }

// ============================================================
//  Helper: cluster <-> LBA conversion
// ============================================================
static uint32_t cluster_to_lba(uint32_t cluster) {
    return fat32.data_lba + (cluster - 2) * fat32.sectors_per_cluster;
}

// ============================================================
//  Helper: read / write whole cluster
// ============================================================
static int read_cluster(uint32_t cluster, uint8_t* buf) {
    uint32_t lba = cluster_to_lba(cluster);
    for (uint32_t s = 0; s < fat32.sectors_per_cluster; s++) {
        if (!disk_read_sector64(lba + s, buf + s * FAT32_SECTOR_SIZE))
            return 0;
    }
    return 1;
}
static int write_cluster(uint32_t cluster, const uint8_t* buf) {
    uint32_t lba = cluster_to_lba(cluster);
    for (uint32_t s = 0; s < fat32.sectors_per_cluster; s++) {
        if (!disk_write_sector64(lba + s, buf + s * FAT32_SECTOR_SIZE))
            return 0;
    }
    return 1;
}

// ============================================================
//  Helper: FAT entry read / write
// ============================================================
static uint8_t fat_sector_buf[FAT32_SECTOR_SIZE];

static uint32_t fat_read_entry(uint32_t cluster) {
    uint32_t fat_offset   = cluster * 4;
    uint32_t fat_sector   = fat32.fat_lba + fat_offset / FAT32_SECTOR_SIZE;
    uint32_t entry_offset = fat_offset % FAT32_SECTOR_SIZE;
    if (!disk_read_sector64(fat_sector, fat_sector_buf)) return FAT32_BAD_CLUSTER;
    return read_le32(fat_sector_buf + entry_offset) & 0x0FFFFFFF;
}

static int fat_write_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset   = cluster * 4;
    uint32_t fat_sector   = fat32.fat_lba + fat_offset / FAT32_SECTOR_SIZE;
    uint32_t entry_offset = fat_offset % FAT32_SECTOR_SIZE;
    if (!disk_read_sector64(fat_sector, fat_sector_buf)) return 0;
    write_le32(fat_sector_buf + entry_offset, value & 0x0FFFFFFF);
    if (!disk_write_sector64(fat_sector, fat_sector_buf)) return 0;
    /* mirror to FAT2 */
    uint32_t fat2_sector = fat32.fat2_lba + fat_offset / FAT32_SECTOR_SIZE;
    disk_write_sector64(fat2_sector, fat_sector_buf);
    return 1;
}

// ============================================================
//  Helper: allocate a free cluster
// ============================================================
static uint32_t fat_alloc_cluster(void) {
    for (uint32_t c = 2; c < fat32.total_clusters + 2; c++) {
        if (fat_read_entry(c) == FAT32_FREE_CLUSTER) {
            fat_write_entry(c, FAT32_EOC);
            return c;
        }
    }
    return 0;
}

// ============================================================
//  Helper: free an entire cluster chain
// ============================================================
static void fat_free_chain(uint32_t start) {
    uint32_t c = start;
    while (c >= 2 && c < FAT32_EOC) {
        uint32_t next = fat_read_entry(c);
        fat_write_entry(c, FAT32_FREE_CLUSTER);
        c = next;
    }
}

// ============================================================
//  Helper: 8.3 name conversion
// ============================================================
static void name_to_83(const char* in, uint8_t out[11]) {
    k_memset(out, ' ', 11);
    uint32_t i = 0, o = 0;
    while (in[i] && in[i] != '.' && o < 8) {
        uint8_t c = (uint8_t)in[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[o++] = c;
    }
    while (in[i] && in[i] != '.') i++;
    if (in[i] == '.') {
        i++;
        uint32_t e = 8;
        while (in[i] && e < 11) {
            uint8_t c = (uint8_t)in[i++];
            if (c >= 'a' && c <= 'z') c -= 32;
            out[e++] = c;
        }
    }
}

// ============================================================
//  Helper: root directory operations
// ============================================================
static uint8_t dir_cluster_buf[FAT32_SECTOR_SIZE * 8];

static uint32_t root_find(const char* name83, Fat32DirEntry* out) {
    if (!fat32.mounted) return (uint32_t)-1;

    uint8_t name83_buf[11];
    name_to_83(name83, name83_buf);

    uint32_t entries_per_cluster = 
        fat32.sectors_per_cluster * FAT32_SECTOR_SIZE / 32;
    uint32_t cluster = fat32.root_cluster;
    uint32_t index = 0;

    while (cluster >= 2 && cluster < FAT32_EOC) {
        if (!read_cluster(cluster, dir_cluster_buf)) return (uint32_t)-1;
        
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            Fat32DirEntry* de = (Fat32DirEntry*)(dir_cluster_buf + i * 32);
            
            if (de->name[0] == 0x00) return (uint32_t)-1;
            if (de->name[0] == 0xE5) { index++; continue; }
            if (de->attr == FAT32_ATTR_LFN) { index++; continue; }
            if (de->attr & FAT32_ATTR_VOLUME_ID) { index++; continue; }
            
            if (k_memcmp(de->name, name83_buf, 11) == 0) {
                if (out) k_memcpy(out, de, sizeof(Fat32DirEntry));
                return index;
            }
            index++;
        }
        cluster = fat_read_entry(cluster);
    }
    return (uint32_t)-1;
}

static uint32_t root_alloc_slot(void) {
    if (!fat32.mounted) return (uint32_t)-1;

    uint32_t entries_per_cluster = 
        fat32.sectors_per_cluster * FAT32_SECTOR_SIZE / 32;
    uint32_t cluster = fat32.root_cluster;
    uint32_t index = 0;

    while (cluster >= 2 && cluster < FAT32_EOC) {
        if (!read_cluster(cluster, dir_cluster_buf)) return (uint32_t)-1;
        
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            Fat32DirEntry* de = (Fat32DirEntry*)(dir_cluster_buf + i * 32);
            
            if (de->name[0] == 0x00 || de->name[0] == 0xE5) {
                return index;
            }
            index++;
        }
        
        uint32_t next = fat_read_entry(cluster);
        if (next >= FAT32_EOC) {
            uint32_t new_cluster = fat_alloc_cluster();
            if (!new_cluster) return (uint32_t)-1;
            
            fat_write_entry(cluster, new_cluster);
            k_memset(dir_cluster_buf, 0, fat32.sectors_per_cluster * FAT32_SECTOR_SIZE);
            if (!write_cluster(new_cluster, dir_cluster_buf)) return (uint32_t)-1;
            return index;
        }
        cluster = next;
    }
    return (uint32_t)-1;
}

static int root_write_entry(uint32_t index, const Fat32DirEntry* de) {
    if (!fat32.mounted) return 0;

    uint32_t entries_per_cluster = 
        fat32.sectors_per_cluster * FAT32_SECTOR_SIZE / 32;
    uint32_t cluster_idx = index / entries_per_cluster;
    uint32_t entry_in_cluster = index % entries_per_cluster;
    
    uint32_t cluster = fat32.root_cluster;
    for (uint32_t i = 0; i < cluster_idx; i++) {
        cluster = fat_read_entry(cluster);
        if (cluster < 2 || cluster >= FAT32_EOC) return 0;
    }
    
    if (!read_cluster(cluster, dir_cluster_buf)) return 0;
    k_memcpy(dir_cluster_buf + entry_in_cluster * 32, de, sizeof(Fat32DirEntry));
    return write_cluster(cluster, dir_cluster_buf);
}

// ============================================================
//  fat32_mount()
// ============================================================
int fat32_mount(void) {
    uint8_t buf[FAT32_SECTOR_SIZE];
    
    if (!disk_read_sector64(FAT32_PARTITION_LBA, buf)) return 0;
    
    /* Validate boot signature */
    if (buf[510] != 0x55 || buf[511] != 0xAA) return 0;
    
    /* Read BPB fields manually */
    uint16_t bytes_per_sector = read_le16(buf + 11);
    uint8_t  sectors_per_cluster = buf[13];
    uint16_t reserved_sectors = read_le16(buf + 14);
    uint8_t  num_fats = buf[16];
    uint32_t total_sectors_32 = read_le32(buf + 32);
    uint32_t fat_size_32 = read_le32(buf + 36);
    uint32_t root_cluster = read_le32(buf + 44);
    
    /* Validate FAT32 signature */
    if (k_memcmp(buf + 82, "FAT32   ", 8) != 0) return 0;
    
    /* Validate parameters */
    if (bytes_per_sector != 512) return 0;
    if (sectors_per_cluster == 0) return 0;
    if (root_cluster < 2) return 0;
    
    /* Cache geometry */
    fat32.partition_lba = FAT32_PARTITION_LBA;
    fat32.sectors_per_cluster = sectors_per_cluster;
    fat32.fat_size_sectors = fat_size_32;
    fat32.root_cluster = root_cluster;
    
    fat32.fat_lba = fat32.partition_lba + reserved_sectors;
    fat32.fat2_lba = fat32.fat_lba + fat_size_32;
    fat32.data_lba = fat32.fat_lba + (num_fats * fat_size_32);
    
    fat32.total_clusters = (total_sectors_32 - 
                           (fat32.data_lba - fat32.partition_lba)) 
                           / sectors_per_cluster;
    
    fat32.mounted = 1;
    return 1;
}

// ============================================================
//  fat32_format() - OPTIMIZED FOR 2GB DISK
// ============================================================
int fat32_format(void) {
    uint8_t buf[FAT32_SECTOR_SIZE];
    k_memset(buf, 0, FAT32_SECTOR_SIZE);
    
    // Optimized geometry for 2GB disk:
    // - 8 sectors per cluster = 4KB clusters
    // - More efficient for larger files
    // - Still good for small files
    uint32_t reserved    = 32;
    uint32_t fat_secs    = 2048;        // Larger FAT for more clusters
    uint32_t num_fats    = 2;
    uint32_t total_secs  = 4194304;     // 2GB / 512 bytes = 4M sectors
    uint32_t secs_per_cluster = 8;      // 4KB clusters
    uint32_t root_clus   = 2;
    
    /* Jump boot */
    buf[0] = 0xEB; buf[1] = 0x58; buf[2] = 0x90;
    
    /* OEM name */
    k_memcpy(buf + 3, "ASCENTOS", 8);
    
    /* BPB - write fields manually */
    write_le16(buf + 11, 512);              // bytes_per_sector
    buf[13] = secs_per_cluster;             // sectors_per_cluster = 8
    write_le16(buf + 14, reserved);         // reserved_sectors
    buf[16] = num_fats;                     // num_fats
    write_le16(buf + 17, 0);                // root_entry_count
    write_le16(buf + 19, 0);                // total_sectors_16
    buf[21] = 0xF8;                         // media_type
    write_le16(buf + 22, 0);                // fat_size_16
    write_le16(buf + 24, 63);               // sectors_per_track
    write_le16(buf + 26, 255);              // num_heads
    write_le32(buf + 28, 0);                // hidden_sectors
    write_le32(buf + 32, total_secs);       // total_sectors_32
    
    /* FAT32 extended BPB */
    write_le32(buf + 36, fat_secs);         // fat_size_32
    write_le16(buf + 40, 0);                // ext_flags
    write_le16(buf + 42, 0);                // fs_version
    write_le32(buf + 44, root_clus);        // root_cluster = 2
    write_le16(buf + 48, 1);                // fs_info_sector
    write_le16(buf + 50, 6);                // backup_boot_sector
    buf[64] = 0x80;                         // drive_number
    buf[66] = 0x29;                         // boot_signature
    write_le32(buf + 67, 0xDEADBEEF);       // volume_id
    k_memcpy(buf + 71, "ASCENTOS   ", 11);  // volume_label
    k_memcpy(buf + 82, "FAT32   ", 8);      // fs_type
    
    /* Boot sector signature */
    buf[510] = 0x55; 
    buf[511] = 0xAA;
    
    if (!disk_write_sector64(FAT32_PARTITION_LBA, buf)) return 0;
    
    /* Zero out the FATs */
    k_memset(buf, 0, FAT32_SECTOR_SIZE);
    uint32_t fat_start = FAT32_PARTITION_LBA + reserved;
    for (uint32_t s = 0; s < fat_secs * num_fats; s++)
        disk_write_sector64(fat_start + s, buf);
    
    /* Write FAT entries */
    k_memset(buf, 0, FAT32_SECTOR_SIZE);
    write_le32(buf + 0, 0x0FFFFFF8);
    write_le32(buf + 4, 0x0FFFFFFF);
    write_le32(buf + 8, 0x0FFFFFFF);
    disk_write_sector64(fat_start, buf);
    disk_write_sector64(fat_start + fat_secs, buf);
    
    /* Zero root cluster - need to zero all 8 sectors */
    k_memset(buf, 0, FAT32_SECTOR_SIZE);
    uint32_t data_lba = fat_start + fat_secs * num_fats;
    for (uint32_t s = 0; s < secs_per_cluster; s++) {
        disk_write_sector64(data_lba + s, buf);
    }
    
    return fat32_mount();
}

// ============================================================
//  File operations
// ============================================================
int fat32_create_file(const char* name83) {
    if (!fat32.mounted) return 0;
    if (root_find(name83, NULL) != (uint32_t)-1) return 0;

    uint32_t slot = root_alloc_slot();
    if (slot == (uint32_t)-1) return 0;

    Fat32DirEntry de;
    k_memset(&de, 0, sizeof(de));
    
    uint8_t name83_buf[11];
    name_to_83(name83, name83_buf);
    k_memcpy(de.name, name83_buf, 8);
    k_memcpy(de.ext, name83_buf + 8, 3);
    
    de.attr = FAT32_ATTR_ARCHIVE;
    de.file_size = 0;
    de.fst_clus_hi = 0;
    de.fst_clus_lo = 0;

    return root_write_entry(slot, &de);
}

int fat32_delete_file(const char* name83) {
    if (!fat32.mounted) return 0;

    Fat32DirEntry de;
    uint32_t idx = root_find(name83, &de);
    if (idx == (uint32_t)-1) return 0;

    uint32_t start = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
    if (start >= 2) fat_free_chain(start);

    de.name[0] = 0xE5;
    root_write_entry(idx, &de);
    return 1;
}

int fat32_write_file(const char* name83, const uint8_t* data, uint32_t len) {
    if (!fat32.mounted) return 0;

    Fat32DirEntry de;
    uint32_t idx = root_find(name83, &de);
    if (idx == (uint32_t)-1) return 0;

    uint32_t old_start = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
    if (old_start >= 2) fat_free_chain(old_start);

    de.fst_clus_hi = 0;
    de.fst_clus_lo = 0;
    de.file_size = 0;

    if (len == 0) {
        return root_write_entry(idx, &de);
    }

    uint32_t bytes_per_cluster = fat32.sectors_per_cluster * FAT32_SECTOR_SIZE;
    uint32_t remaining = len;
    uint32_t written = 0;
    uint32_t first_cluster = 0;
    uint32_t prev_cluster = 0;

    static uint8_t wbuf[FAT32_SECTOR_SIZE * 8];

    while (remaining > 0) {
        uint32_t c = fat_alloc_cluster();
        if (!c) return 0;

        if (!first_cluster) first_cluster = c;
        if (prev_cluster) fat_write_entry(prev_cluster, c);

        uint32_t chunk = remaining < bytes_per_cluster ? remaining : bytes_per_cluster;
        k_memset(wbuf, 0, bytes_per_cluster);
        k_memcpy(wbuf, data + written, chunk);
        if (!write_cluster(c, wbuf)) return 0;

        written += chunk;
        remaining -= chunk;
        prev_cluster = c;
    }
    
    if (!fat_write_entry(prev_cluster, FAT32_EOC)) return 0;

    de.fst_clus_hi = (uint16_t)(first_cluster >> 16);
    de.fst_clus_lo = (uint16_t)(first_cluster & 0xFFFF);
    de.file_size = len;
    return root_write_entry(idx, &de);
}

int fat32_read_file(const char* name83, uint8_t* out, uint32_t max_len) {
    if (!fat32.mounted) return -1;

    Fat32DirEntry de;
    if (root_find(name83, &de) == (uint32_t)-1) return -1;

    uint32_t fsize = de.file_size;
    if (fsize == 0) return 0;

    uint32_t start = ((uint32_t)de.fst_clus_hi << 16) | de.fst_clus_lo;
    if (start < 2) return 0;

    uint32_t bytes_per_cluster = fat32.sectors_per_cluster * FAT32_SECTOR_SIZE;
    static uint8_t rbuf[FAT32_SECTOR_SIZE * 8];

    uint32_t total_read = 0;
    uint32_t cluster = start;

    while (cluster >= 2 && cluster < FAT32_EOC && total_read < fsize && total_read < max_len) {
        if (!read_cluster(cluster, rbuf)) return -1;
        uint32_t chunk = fsize - total_read;
        if (chunk > bytes_per_cluster) chunk = bytes_per_cluster;
        if (total_read + chunk > max_len) chunk = max_len - total_read;
        k_memcpy(out + total_read, rbuf, chunk);
        total_read += chunk;
        cluster = fat_read_entry(cluster);
    }
    return (int)total_read;
}

uint32_t fat32_file_size(const char* name83) {
    if (!fat32.mounted) return 0;
    Fat32DirEntry de;
    if (root_find(name83, &de) == (uint32_t)-1) return 0;
    return de.file_size;
}

int fat32_next_entry(uint32_t* idx, Fat32DirEntry* out) {
    if (!fat32.mounted) return 0;

    uint32_t entries_per_cluster =
        fat32.sectors_per_cluster * FAT32_SECTOR_SIZE / 32;
    uint32_t cluster_idx = *idx / entries_per_cluster;
    uint32_t entry_in_clus = *idx % entries_per_cluster;

    uint32_t cluster = fat32.root_cluster;
    for (uint32_t i = 0; i < cluster_idx; i++) {
        cluster = fat_read_entry(cluster);
        if (cluster < 2 || cluster >= FAT32_EOC) return 0;
    }

    while (cluster >= 2 && cluster < FAT32_EOC) {
        if (!read_cluster(cluster, dir_cluster_buf)) return 0;
        while (entry_in_clus < entries_per_cluster) {
            Fat32DirEntry* de =
                (Fat32DirEntry*)(dir_cluster_buf + entry_in_clus * 32);
            (*idx)++;
            entry_in_clus++;

            if (de->name[0] == 0x00) return 0;
            if (de->name[0] == 0xE5) continue;
            if (de->attr == FAT32_ATTR_LFN) continue;
            if (de->attr & FAT32_ATTR_VOLUME_ID) continue;

            k_memcpy(out, de, 32);
            return 1;
        }
        entry_in_clus = 0;
        cluster_idx++;
        cluster = fat_read_entry(cluster);
    }
    return 0;
}