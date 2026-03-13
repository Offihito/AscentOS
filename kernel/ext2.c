// ext2.c — AscentOS Ext2 Filesystem Driver
//
// ATA PIO katmanı: disk_read_sector64 / disk_write_sector64
// Block size: 1024 byte varsayımı ile başlar, superblock'tan okunur.
// Desteklenen özellikler:
//   - Superblock okuma + doğrulama
//   - Block / inode bitmap ile alloc / free
//   - Direkt bloklar (i_block[0..11])
//   - Tek seviye indirect (i_block[12])
//   - Çift seviye indirect (i_block[13]) — büyük dosyalar
//   - Path çözümleme (mutlak + CWD'ye göreli)
//   - Tam CRUD: create, read, write, truncate, unlink, mkdir, rmdir, rename
//   - getdents64, getcwd, chdir

#include "ext2.h"
#include "ata64.h"   // disk_read_sector64 / disk_write_sector64 (ata64.h)
#include <stddef.h>
#include <stdint.h>

extern void serial_print(const char* s);  // kernel64.c

// ============================================================
//  Dahili string / bellek yardımcıları
//  (libc kullanmıyoruz — bare-metal)
// ============================================================
static void e2_memset(void* dst, uint8_t val, uint32_t n) {
    uint8_t* p = (uint8_t*)dst;
    while (n--) *p++ = val;
}
static void e2_memcpy(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
}
static int e2_memcmp(const void* a, const void* b, uint32_t n) {
    const uint8_t* p = (const uint8_t*)a;
    const uint8_t* q = (const uint8_t*)b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}
static int e2_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}
static int e2_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static void e2_strcpy(char* dst, const char* src) {
    while ((*dst++ = *src++));
}
static void e2_strcat(char* dst, const char* src) {
    while (*dst) dst++;
    while ((*dst++ = *src++));
}

// ============================================================
//  Timestamp — unix epoch yaklaşımı
//  RTC sadece saat/dakika/saniye veriyor; tarih yok.
//  Boot anını 2025-01-01 00:00:00 (unix=1735689600) olarak sabitleriz,
//  sonrasını tick sayacıyla ilerletiriz.
// ============================================================
extern void gui_get_rtc_time(uint8_t* h, uint8_t* m, uint8_t* s);

#define EXT2_EPOCH_BASE 1735689600u  // 2025-01-01 00:00:00 UTC

static uint32_t ext2_boot_rtc_seconds = 0;   // boot anındaki RTC toplam saniye
static uint32_t ext2_boot_unix        = 0;   // boot anındaki unix timestamp

// Boot sırasında bir kez çağrılır (ext2_mount içinde)
static void ext2_init_clock(void) {
    uint8_t h = 0, m = 0, s = 0;
    gui_get_rtc_time(&h, &m, &s);
    ext2_boot_rtc_seconds = (uint32_t)h * 3600u + (uint32_t)m * 60u + (uint32_t)s;
    ext2_boot_unix        = EXT2_EPOCH_BASE + ext2_boot_rtc_seconds;
}

// Mevcut unix timestamp'i döndürür
static uint32_t ext2_now(void) {
    uint8_t h = 0, m = 0, s = 0;
    gui_get_rtc_time(&h, &m, &s);
    uint32_t cur = (uint32_t)h * 3600u + (uint32_t)m * 60u + (uint32_t)s;
    // Gün geçişi — saniyeler geriye giderse +86400 ekle
    uint32_t delta = (cur >= ext2_boot_rtc_seconds)
                   ? (cur - ext2_boot_rtc_seconds)
                   : (86400u - ext2_boot_rtc_seconds + cur);
    return ext2_boot_unix + delta;
}

// ============================================================
//  Superblock I/O
// ============================================================
static int read_superblock(Ext2Superblock* out) {
    static uint8_t sb_buf[EXT2_SECTOR_SIZE * 2];
    if (!disk_read_sector64(EXT2_SUPERBLOCK_LBA,     sb_buf))       return 0;
    if (!disk_read_sector64(EXT2_SUPERBLOCK_LBA + 1, sb_buf + 512)) return 0;
    e2_memcpy(out, sb_buf, sizeof(Ext2Superblock));
    return 1;
}

static int write_superblock(const Ext2Superblock* in) {
    static uint8_t sb_buf[EXT2_SECTOR_SIZE * 2];
    e2_memset(sb_buf, 0, sizeof(sb_buf));
    e2_memcpy(sb_buf, in, sizeof(Ext2Superblock));
    if (!disk_write_sector64(EXT2_SUPERBLOCK_LBA,     sb_buf))       return 0;
    if (!disk_write_sector64(EXT2_SUPERBLOCK_LBA + 1, sb_buf + 512)) return 0;
    return 1;
}

// Superblock'taki global sayaçları güncelle
static void sb_update_free_blocks(int delta) {
    Ext2Superblock sb;
    if (!read_superblock(&sb)) return;
    if (delta < 0 && sb.s_free_blocks_count < (uint32_t)(-delta)) return;
    sb.s_free_blocks_count = (uint32_t)((int)sb.s_free_blocks_count + delta);
    write_superblock(&sb);
}

static void sb_update_free_inodes(int delta) {
    Ext2Superblock sb;
    if (!read_superblock(&sb)) return;
    if (delta < 0 && sb.s_free_inodes_count < (uint32_t)(-delta)) return;
    sb.s_free_inodes_count = (uint32_t)((int)sb.s_free_inodes_count + delta);
    write_superblock(&sb);
}

// ============================================================
//  Global mount state
// ============================================================
static Ext2State state;

const Ext2State* ext2_get_state(void) { return &state; }

// ============================================================
//  Temel disk I/O — block <-> sektör dönüşümü
// ============================================================

// block_to_lba: 1024-byte block numarasını 512-byte LBA'ya çevir.
// Dikkat: superblock block 1'de (byte 1024), ama bu 0-based block sistemi
// için first_data_block zaten hesaba katılıyor.
static uint32_t block_to_lba(uint32_t block) {
    return block * state.sectors_per_block;
}

// read_block / write_block: tüm block'u (N sektör) oku/yaz.
// static buffer: 4096 byte max block (8 sektör).
#define MAX_BLOCK_SIZE 4096u

static int read_block(uint32_t block, uint8_t* buf) {
    uint32_t lba = block_to_lba(block);
    for (uint32_t s = 0; s < state.sectors_per_block; s++) {
        if (!disk_read_sector64(lba + s, buf + s * EXT2_SECTOR_SIZE))
            return 0;
    }
    return 1;
}

static int write_block(uint32_t block, const uint8_t* buf) {
    uint32_t lba = block_to_lba(block);
    for (uint32_t s = 0; s < state.sectors_per_block; s++) {
        if (!disk_write_sector64(lba + s, buf + s * EXT2_SECTOR_SIZE))
            return 0;
    }
    return 1;
}

// ============================================================
//  Group Descriptor I/O
//  GDT: superblock'tan hemen sonraki block'ta (first_data_block+1)
// ============================================================
static Ext2GroupDesc gdt_cache[64];   // max 64 grup (static)
static int gdt_loaded = 0;

static int load_gdt(void) {
    if (gdt_loaded) return 1;
    // GDT block = first_data_block + 1
    uint32_t gdt_block = state.first_data_block + 1;
    static uint8_t gdt_buf[MAX_BLOCK_SIZE];
    if (!read_block(gdt_block, gdt_buf)) return 0;

    uint32_t n = state.num_groups;
    if (n > 64) n = 64;
    e2_memcpy(gdt_cache, gdt_buf, n * sizeof(Ext2GroupDesc));
    gdt_loaded = 1;
    return 1;
}

static int read_group_desc(uint32_t group, Ext2GroupDesc* out) {
    if (!load_gdt()) return 0;
    if (group >= state.num_groups) return 0;
    *out = gdt_cache[group];
    return 1;
}

static int write_group_desc(uint32_t group, const Ext2GroupDesc* in) {
    if (!load_gdt()) return 0;
    if (group >= state.num_groups) return 0;
    gdt_cache[group] = *in;

    // Diske geri yaz
    uint32_t gdt_block = state.first_data_block + 1;
    static uint8_t gdt_buf[MAX_BLOCK_SIZE];
    if (!read_block(gdt_block, gdt_buf)) return 0;
    e2_memcpy(gdt_buf + group * sizeof(Ext2GroupDesc), in, sizeof(Ext2GroupDesc));
    return write_block(gdt_block, gdt_buf);
}

// ============================================================
//  Inode I/O
// ============================================================
static int read_inode(uint32_t ino, Ext2Inode* out) {
    if (ino == 0 || ino > state.inodes_count) return 0;

    uint32_t group    = (ino - 1) / state.inodes_per_group;
    uint32_t local    = (ino - 1) % state.inodes_per_group;

    Ext2GroupDesc gd;
    if (!read_group_desc(group, &gd)) return 0;

    uint32_t inode_table_block = gd.bg_inode_table;
    // Kaçıncı byte'ta: local * inode_size
    uint32_t byte_offset = local * state.inode_size;
    uint32_t block_off   = byte_offset / state.block_size;
    uint32_t intra_off   = byte_offset % state.block_size;

    static uint8_t inode_buf[MAX_BLOCK_SIZE];
    if (!read_block(inode_table_block + block_off, inode_buf)) return 0;

    e2_memcpy(out, inode_buf + intra_off, sizeof(Ext2Inode));
    return 1;
}

static int write_inode(uint32_t ino, const Ext2Inode* in) {
    if (ino == 0 || ino > state.inodes_count) return 0;

    uint32_t group    = (ino - 1) / state.inodes_per_group;
    uint32_t local    = (ino - 1) % state.inodes_per_group;

    Ext2GroupDesc gd;
    if (!read_group_desc(group, &gd)) return 0;

    uint32_t inode_table_block = gd.bg_inode_table;
    uint32_t byte_offset = local * state.inode_size;
    uint32_t block_off   = byte_offset / state.block_size;
    uint32_t intra_off   = byte_offset % state.block_size;

    static uint8_t inode_buf[MAX_BLOCK_SIZE];
    if (!read_block(inode_table_block + block_off, inode_buf)) return 0;
    e2_memcpy(inode_buf + intra_off, in, sizeof(Ext2Inode));
    return write_block(inode_table_block + block_off, inode_buf);
}

// ============================================================
//  Block bitmap — alloc / free
// ============================================================
static uint32_t alloc_block(void) {
    for (uint32_t g = 0; g < state.num_groups; g++) {
        Ext2GroupDesc gd;
        if (!read_group_desc(g, &gd)) continue;
        if (gd.bg_free_blocks_count == 0) continue;

        static uint8_t bmap[MAX_BLOCK_SIZE];
        if (!read_block(gd.bg_block_bitmap, bmap)) continue;

        uint32_t limit = state.blocks_per_group;
        // son grup daha az block içerebilir
        uint32_t used_blocks = g * state.blocks_per_group;
        if (used_blocks + limit > state.blocks_count)
            limit = state.blocks_count - used_blocks;

        for (uint32_t b = 0; b < limit; b++) {
            uint32_t byte_i = b / 8;
            uint8_t  bit_i  = b % 8;
            if (!(bmap[byte_i] & (1u << bit_i))) {
                // Boş bit bulundu — işaretle
                bmap[byte_i] |= (1u << bit_i);
                if (!write_block(gd.bg_block_bitmap, bmap)) return 0;
                gd.bg_free_blocks_count--;
                write_group_desc(g, &gd);
                sb_update_free_blocks(-1);
                return g * state.blocks_per_group + b + state.first_data_block;
            }
        }
    }
    return 0;  // disk dolu
}

static int free_block(uint32_t block) {
    if (block < state.first_data_block) return 0;
    uint32_t rel = block - state.first_data_block;
    uint32_t g   = rel / state.blocks_per_group;
    uint32_t b   = rel % state.blocks_per_group;

    if (g >= state.num_groups) return 0;

    Ext2GroupDesc gd;
    if (!read_group_desc(g, &gd)) return 0;

    static uint8_t bmap[MAX_BLOCK_SIZE];
    if (!read_block(gd.bg_block_bitmap, bmap)) return 0;

    bmap[b / 8] &= ~(1u << (b % 8));
    if (!write_block(gd.bg_block_bitmap, bmap)) return 0;

    gd.bg_free_blocks_count++;
    write_group_desc(g, &gd);
    sb_update_free_blocks(+1);
    return 1;
}

// ============================================================
//  Inode bitmap — alloc / free
// ============================================================
static uint32_t alloc_inode(void) {
    for (uint32_t g = 0; g < state.num_groups; g++) {
        Ext2GroupDesc gd;
        if (!read_group_desc(g, &gd)) continue;
        if (gd.bg_free_inodes_count == 0) continue;

        static uint8_t imap[MAX_BLOCK_SIZE];
        if (!read_block(gd.bg_inode_bitmap, imap)) continue;

        for (uint32_t b = 0; b < state.inodes_per_group; b++) {
            if (!(imap[b / 8] & (1u << (b % 8)))) {
                imap[b / 8] |= (1u << (b % 8));
                if (!write_block(gd.bg_inode_bitmap, imap)) return 0;
                gd.bg_free_inodes_count--;
                write_group_desc(g, &gd);
                sb_update_free_inodes(-1);
                return g * state.inodes_per_group + b + 1;  // 1-based
            }
        }
    }
    return 0;
}

static int free_inode(uint32_t ino) {
    if (ino == 0 || ino > state.inodes_count) return 0;
    uint32_t g = (ino - 1) / state.inodes_per_group;
    uint32_t b = (ino - 1) % state.inodes_per_group;

    if (g >= state.num_groups) return 0;

    Ext2GroupDesc gd;
    if (!read_group_desc(g, &gd)) return 0;

    static uint8_t imap[MAX_BLOCK_SIZE];
    if (!read_block(gd.bg_inode_bitmap, imap)) return 0;

    imap[b / 8] &= ~(1u << (b % 8));
    if (!write_block(gd.bg_inode_bitmap, imap)) return 0;

    gd.bg_free_inodes_count++;
    write_group_desc(g, &gd);
    sb_update_free_inodes(+1);
    return 1;
}

// ============================================================
//  Indirect block okuma yardımcısı
//  Inode mantıksal blok indeksinden fiziksel block numarasını döndürür.
//  Döner: 0 = eşleşme yok / hata
// ============================================================
static uint32_t read_file_block(const Ext2Inode* ino, uint32_t blk_idx) {
    uint32_t ptrs_per_block = state.block_size / 4;

    // Direkt bloklar [0..11]
    if (blk_idx < 12) {
        return ino->i_block[blk_idx];
    }

    // Tek seviye indirect [12]
    blk_idx -= 12;
    if (blk_idx < ptrs_per_block) {
        if (!ino->i_block[12]) return 0;
        static uint8_t ind_buf[MAX_BLOCK_SIZE];
        if (!read_block(ino->i_block[12], ind_buf)) return 0;
        uint32_t* ptrs = (uint32_t*)ind_buf;
        return ptrs[blk_idx];
    }

    // Çift seviye indirect [13]
    blk_idx -= ptrs_per_block;
    if (blk_idx < ptrs_per_block * ptrs_per_block) {
        if (!ino->i_block[13]) return 0;
        static uint8_t dind_buf[MAX_BLOCK_SIZE];
        if (!read_block(ino->i_block[13], dind_buf)) return 0;
        uint32_t* l1 = (uint32_t*)dind_buf;
        uint32_t l1_idx = blk_idx / ptrs_per_block;
        uint32_t l2_idx = blk_idx % ptrs_per_block;
        if (!l1[l1_idx]) return 0;
        static uint8_t dind2_buf[MAX_BLOCK_SIZE];
        if (!read_block(l1[l1_idx], dind2_buf)) return 0;
        uint32_t* l2 = (uint32_t*)dind2_buf;
        return l2[l2_idx];
    }

    // Üçlü seviye (tindirect) — desteklenmez (pratik sınır ~64 MB)
    return 0;
}

// ============================================================
//  Inode'a yeni blok tahsis et ve yaz
//  blk_idx: inode'daki mantıksal blok indeksi
//  Döner: fiziksel block no | 0 (hata)
// ============================================================
static uint32_t alloc_file_block(Ext2Inode* ino, uint32_t blk_idx) {
    uint32_t ptrs_per_block = state.block_size / 4;

    uint32_t phys = alloc_block();
    if (!phys) return 0;

    // Yeni bloğu sıfırla
    static uint8_t zero_buf[MAX_BLOCK_SIZE];
    e2_memset(zero_buf, 0, state.block_size);
    write_block(phys, zero_buf);

    // Direkt bloklar
    if (blk_idx < 12) {
        ino->i_block[blk_idx] = phys;
        return phys;
    }

    // Tek seviye indirect
    blk_idx -= 12;
    if (blk_idx < ptrs_per_block) {
        // Indirect block yoksa yeni alloc et
        if (!ino->i_block[12]) {
            uint32_t ind = alloc_block();
            if (!ind) { free_block(phys); return 0; }
            e2_memset(zero_buf, 0, state.block_size);
            write_block(ind, zero_buf);
            ino->i_block[12] = ind;
        }
        static uint8_t ind_buf[MAX_BLOCK_SIZE];
        if (!read_block(ino->i_block[12], ind_buf)) { free_block(phys); return 0; }
        ((uint32_t*)ind_buf)[blk_idx] = phys;
        if (!write_block(ino->i_block[12], ind_buf)) { free_block(phys); return 0; }
        return phys;
    }

    // Çift seviye indirect
    blk_idx -= ptrs_per_block;
    if (blk_idx < ptrs_per_block * ptrs_per_block) {
        uint32_t l1_idx = blk_idx / ptrs_per_block;
        uint32_t l2_idx = blk_idx % ptrs_per_block;

        if (!ino->i_block[13]) {
            uint32_t dind = alloc_block();
            if (!dind) { free_block(phys); return 0; }
            e2_memset(zero_buf, 0, state.block_size);
            write_block(dind, zero_buf);
            ino->i_block[13] = dind;
        }
        static uint8_t dind_buf[MAX_BLOCK_SIZE];
        if (!read_block(ino->i_block[13], dind_buf)) { free_block(phys); return 0; }
        uint32_t* l1 = (uint32_t*)dind_buf;

        if (!l1[l1_idx]) {
            uint32_t l2blk = alloc_block();
            if (!l2blk) { free_block(phys); return 0; }
            e2_memset(zero_buf, 0, state.block_size);
            write_block(l2blk, zero_buf);
            l1[l1_idx] = l2blk;
            write_block(ino->i_block[13], dind_buf);
        }

        static uint8_t dind2_buf[MAX_BLOCK_SIZE];
        if (!read_block(l1[l1_idx], dind2_buf)) { free_block(phys); return 0; }
        ((uint32_t*)dind2_buf)[l2_idx] = phys;
        if (!write_block(l1[l1_idx], dind2_buf)) { free_block(phys); return 0; }
        return phys;
    }

    // Desteklenmeyen seviye
    free_block(phys);
    return 0;
}

// ============================================================
//  Inode'un tüm veri bloklarını serbest bırak
// ============================================================
static void free_inode_blocks(Ext2Inode* ino) {
    uint32_t ptrs_per_block = state.block_size / 4;

    // Direkt
    for (int i = 0; i < 12; i++) {
        if (ino->i_block[i]) {
            free_block(ino->i_block[i]);
            ino->i_block[i] = 0;
        }
    }

    // Tek seviye indirect
    if (ino->i_block[12]) {
        static uint8_t ind_buf[MAX_BLOCK_SIZE];
        if (read_block(ino->i_block[12], ind_buf)) {
            uint32_t* ptrs = (uint32_t*)ind_buf;
            for (uint32_t i = 0; i < ptrs_per_block; i++)
                if (ptrs[i]) free_block(ptrs[i]);
        }
        free_block(ino->i_block[12]);
        ino->i_block[12] = 0;
    }

    // Çift seviye indirect
    if (ino->i_block[13]) {
        static uint8_t dind_buf[MAX_BLOCK_SIZE];
        if (read_block(ino->i_block[13], dind_buf)) {
            uint32_t* l1 = (uint32_t*)dind_buf;
            for (uint32_t i = 0; i < ptrs_per_block; i++) {
                if (!l1[i]) continue;
                static uint8_t dind2_buf[MAX_BLOCK_SIZE];
                if (read_block(l1[i], dind2_buf)) {
                    uint32_t* l2 = (uint32_t*)dind2_buf;
                    for (uint32_t j = 0; j < ptrs_per_block; j++)
                        if (l2[j]) free_block(l2[j]);
                }
                free_block(l1[i]);
            }
        }
        free_block(ino->i_block[13]);
        ino->i_block[13] = 0;
    }
}

// ============================================================
//  Path çözümleme
// ============================================================

// İleri bildirim
static uint32_t dir_lookup(uint32_t dir_ino, const char* name);

// path_resolve:
//   want_parent=0: hedef dosya/dizin inode'unu döndürür
//   want_parent=1: parent dizin inode'unu döndürür, leaf_out'a son bileşeni yazar
// Döner: inode no | 0 (bulunamadı)
static uint32_t path_resolve(const char* path, int want_parent, char* leaf_out) {
    if (!path) return 0;

    // Göreli yol: CWD ile birleştir
    char full[EXT2_MAX_PATH];
    if (path[0] != '/') {
        e2_strcpy(full, state.cwd);
        int cwdlen = e2_strlen(full);
        if (cwdlen > 1) { full[cwdlen] = '/'; full[cwdlen + 1] = '\0'; }
        e2_strcat(full, path);
    } else {
        e2_strcpy(full, path);
    }

    // Baştaki '/'
    const char* p = full;
    while (*p == '/') p++;

    if (*p == '\0') {
        // Root dizini
        if (want_parent) return 0;
        return EXT2_ROOT_INO;
    }

    uint32_t cur_ino = EXT2_ROOT_INO;

    while (*p) {
        // Bileşeni çıkar
        char comp[EXT2_MAX_NAME + 1];
        int  len = 0;
        while (*p && *p != '/' && len < (int)EXT2_MAX_NAME)
            comp[len++] = *p++;
        comp[len] = '\0';
        while (*p == '/') p++;  // ardışık slash'ları atla

        int is_last = (*p == '\0');

        if (want_parent && is_last) {
            if (leaf_out) e2_strcpy(leaf_out, comp);
            return cur_ino;
        }

        // "." → aynı dizin
        if (comp[0] == '.' && comp[1] == '\0') {
            if (is_last) return cur_ino;
            continue;
        }
        // ".." → parent (dir_lookup ile bul)
        uint32_t next = dir_lookup(cur_ino, comp);
        if (!next) return 0;

        if (is_last) return next;

        cur_ino = next;
    }
    return cur_ino;
}

// ============================================================
//  Dizin entry okuma yardımcıları
// ============================================================

// dir_lookup: dir_ino'daki name isimli entry'nin inode'unu döndürür. 0=yok
static uint32_t dir_lookup(uint32_t dir_ino, const char* name) {
    Ext2Inode dir_inode;
    if (!read_inode(dir_ino, &dir_inode)) return 0;
    if (!(dir_inode.i_mode & EXT2_S_IFDIR)) return 0;

    uint32_t size    = dir_inode.i_size;
    uint32_t offset  = 0;
    int      name_len = e2_strlen(name);

    static uint8_t dblk[MAX_BLOCK_SIZE];

    uint32_t blk_idx = 0;
    while (offset < size) {
        uint32_t phys = read_file_block(&dir_inode, blk_idx++);
        if (!phys) break;
        if (!read_block(phys, dblk)) break;

        uint32_t intra = 0;
        while (intra < state.block_size && offset < size) {
            Ext2DirEntry* de = (Ext2DirEntry*)(dblk + intra);
            if (de->rec_len == 0) break;

            if (de->inode && de->name_len == (uint8_t)name_len &&
                e2_memcmp(de->name, name, (uint32_t)name_len) == 0) {
                return de->inode;
            }

            intra  += de->rec_len;
            offset += de->rec_len;
        }
    }
    return 0;
}

// ============================================================
//  Dizin entry ekleme
// ============================================================
static int dir_add_entry(uint32_t dir_ino, const char* name,
                          uint32_t child_ino, uint8_t file_type) {
    Ext2Inode dir_inode;
    if (!read_inode(dir_ino, &dir_inode)) return -1;

    int    name_len   = e2_strlen(name);
    int    need_len   = (int)((8 + name_len + 3) & ~3u);  // 4-byte hizalı
    uint32_t dir_size = dir_inode.i_size;

    static uint8_t dblk[MAX_BLOCK_SIZE];

    // Mevcut bloklarda yer ara
    uint32_t blk_idx = 0;
    uint32_t offset  = 0;
    while (offset < dir_size) {
        uint32_t phys = read_file_block(&dir_inode, blk_idx);
        if (!phys) break;
        if (!read_block(phys, dblk)) break;

        uint32_t intra = 0;
        int modified = 0;

        while (intra < state.block_size && offset < dir_size) {
            Ext2DirEntry* de = (Ext2DirEntry*)(dblk + intra);
            if (de->rec_len == 0) break;

            // Bu entry'nin gerçek boyutu
            int real_len = (int)((8 + de->name_len + 3) & ~3u);
            if (!de->inode) real_len = 0;  // silinmiş entry: tüm rec_len boş

            int free_space = (int)de->rec_len - real_len;
            if (free_space >= need_len) {
                // Yeterli boşluk var: mevcut entry'yi sıkıştır, yeni entry ekle
                if (de->inode) {
                    // Mevcut entry'yi kısalt
                    de->rec_len = (uint16_t)real_len;
                    intra += real_len;
                }
                Ext2DirEntry* ne = (Ext2DirEntry*)(dblk + intra);
                ne->inode     = child_ino;
                ne->rec_len   = (uint16_t)(free_space > 0 ? free_space : need_len);
                ne->name_len  = (uint8_t)name_len;
                ne->file_type = file_type;
                e2_memcpy(ne->name, name, (uint32_t)name_len);
                modified = 1;
                break;
            }

            intra  += de->rec_len;
            offset += de->rec_len;
        }

        if (modified) {
            if (!write_block(phys, dblk)) return -1;
            return 0;
        }

        blk_idx++;
        offset = blk_idx * state.block_size;
    }

    // Mevcut bloklarda yer yok — yeni blok tahsis et
    uint32_t new_phys = alloc_file_block(&dir_inode, blk_idx);
    if (!new_phys) return -1;

    e2_memset(dblk, 0, state.block_size);
    Ext2DirEntry* ne = (Ext2DirEntry*)dblk;
    ne->inode     = child_ino;
    ne->rec_len   = (uint16_t)state.block_size;  // tüm block bu entry'ye ait
    ne->name_len  = (uint8_t)name_len;
    ne->file_type = file_type;
    e2_memcpy(ne->name, name, (uint32_t)name_len);

    if (!write_block(new_phys, dblk)) { free_block(new_phys); return -1; }

    // Inode boyutunu güncelle
    dir_inode.i_size += state.block_size;
    write_inode(dir_ino, &dir_inode);
    return 0;
}

// ============================================================
//  Dizin entry silme
// ============================================================
static int dir_remove_entry(uint32_t dir_ino, const char* name) {
    Ext2Inode dir_inode;
    if (!read_inode(dir_ino, &dir_inode)) return -1;

    int    name_len  = e2_strlen(name);
    uint32_t dir_size = dir_inode.i_size;

    static uint8_t dblk[MAX_BLOCK_SIZE];

    uint32_t blk_idx = 0;
    uint32_t offset  = 0;
    while (offset < dir_size) {
        uint32_t phys = read_file_block(&dir_inode, blk_idx++);
        if (!phys) break;
        if (!read_block(phys, dblk)) break;

        uint32_t intra = 0;
        Ext2DirEntry* prev = NULL;

        while (intra < state.block_size && offset < dir_size) {
            Ext2DirEntry* de = (Ext2DirEntry*)(dblk + intra);
            if (de->rec_len == 0) break;

            if (de->inode && de->name_len == (uint8_t)name_len &&
                e2_memcmp(de->name, name, (uint32_t)name_len) == 0) {
                // Bulundu: entry'yi "silinmiş" işaretle
                if (prev) {
                    // Önceki entry'nin rec_len'ini genişlet
                    prev->rec_len = (uint16_t)(prev->rec_len + de->rec_len);
                } else {
                    // İlk entry ise inode'u 0 yap
                    de->inode = 0;
                }
                write_block(phys, dblk);
                return 0;
            }

            prev   = de;
            intra  += de->rec_len;
            offset += de->rec_len;
        }
    }
    return -1;  // bulunamadı
}

// ============================================================
//  ext2_format
//  Boş bir diske minimal ext2 yapısı yazar:
//    Block 0   : (boot sektörü — dokunulmaz)
//    Block 1   : Superblock  (byte offset 1024)
//    Block 2   : Group Descriptor Table
//    Block 3   : Block bitmap
//    Block 4   : Inode bitmap
//    Block 5.. : Inode table  (inodes_per_group / (block_size/inode_size) block)
//    Sonrası   : Data bölgesi  (root dizin inode=2 buradadır)
//
//  Sabit geometri (64 MB disk için):
//    Block size    : 1024 byte
//    Blocks/group  : 8192
//    Inodes/group  : 1024
//    Toplam block  : 65536  (64 MB / 1024)
//
//  Döner: 0 başarı, -1 hata
// ============================================================
int ext2_format(void) {
    serial_print("[EXT2] Formatting disk...\n");

    // ── Sabit geometri ───────────────────────────────────────
    const uint32_t BLOCK_SIZE        = 1024u;
    const uint32_t SECTORS_PER_BLOCK = BLOCK_SIZE / EXT2_SECTOR_SIZE;  // 2
    const uint32_t TOTAL_BLOCKS      = 65536u;   // 64 MB
    const uint32_t BLOCKS_PER_GROUP  = 8192u;
    const uint32_t INODES_PER_GROUP  = 1024u;
    const uint32_t INODE_SIZE        = EXT2_INODE_SIZE_REV0;  // 128 byte
    const uint32_t FIRST_DATA_BLOCK  = 1u;       // 1024-byte block'larda SB block=1
    const uint32_t INODES_PER_BLOCK  = BLOCK_SIZE / INODE_SIZE;  // 8
    const uint32_t INODE_TABLE_BLOCKS= INODES_PER_GROUP / INODES_PER_BLOCK; // 128

    // Grup sayısı (tek grup: 65536 block / 8192 = 8 grup — ama biz 1 grup ile başlıyoruz)
    // Basitlik için tek grup kullanalım (8192 block = 8 MB veri alanı yeterli)
    const uint32_t NUM_GROUPS        = 1u;
    const uint32_t TOTAL_INODES      = INODES_PER_GROUP * NUM_GROUPS;

    // Tek grup layout (block numaraları):
    //   1 = Superblock
    //   2 = GDT
    //   3 = Block bitmap
    //   4 = Inode bitmap
    //   5..132 = Inode table  (128 block)
    //   133..  = Data bölgesi
    const uint32_t BLK_SUPER   = 1u;
    const uint32_t BLK_GDT     = 2u;
    const uint32_t BLK_BMAP    = 3u;
    const uint32_t BLK_IMAP    = 4u;
    const uint32_t BLK_ITABLE  = 5u;
    const uint32_t BLK_DATA    = BLK_ITABLE + INODE_TABLE_BLOCKS;  // 133

    // Meta block sayısı (superblock dahil)
    const uint32_t META_BLOCKS  = BLK_DATA - FIRST_DATA_BLOCK;  // 132
    const uint32_t FREE_BLOCKS  = BLOCKS_PER_GROUP - META_BLOCKS - 1u;  // root dir 1 block alır

    static uint8_t buf[1024];

    // ── 1. Superblock ─────────────────────────────────────────
    e2_memset(buf, 0, sizeof(buf));
    Ext2Superblock* sb = (Ext2Superblock*)buf;

    sb->s_inodes_count      = TOTAL_INODES;
    sb->s_blocks_count      = TOTAL_BLOCKS;
    sb->s_r_blocks_count    = 0;
    sb->s_free_blocks_count = FREE_BLOCKS;
    sb->s_free_inodes_count = TOTAL_INODES - 11u;  // 1-10 rezerve, 11=ilk serbest, 2=root
    sb->s_first_data_block  = FIRST_DATA_BLOCK;
    sb->s_log_block_size    = 0;   // 1024 << 0 = 1024
    sb->s_log_frag_size     = 0;
    sb->s_blocks_per_group  = BLOCKS_PER_GROUP;
    sb->s_frags_per_group   = BLOCKS_PER_GROUP;
    sb->s_inodes_per_group  = INODES_PER_GROUP;
    sb->s_mtime             = 0;
    sb->s_wtime             = 0;
    sb->s_mnt_count         = 0;
    sb->s_max_mnt_count     = 20;
    sb->s_magic             = EXT2_SUPER_MAGIC;
    sb->s_state             = 1;   // EXT2_VALID_FS
    sb->s_errors            = 1;   // EXT2_ERRORS_CONTINUE
    sb->s_rev_level         = 0;   // old revision — basit
    sb->s_first_ino         = 11u;
    sb->s_inode_size        = (uint16_t)INODE_SIZE;

    // Superblock LBA 2-3'e yaz (byte offset 1024 = LBA 2)
    if (!disk_write_sector64(EXT2_SUPERBLOCK_LBA,     buf))         return -1;
    if (!disk_write_sector64(EXT2_SUPERBLOCK_LBA + 1, buf + 512))   return -1;

    // ── 2. Group Descriptor Table ─────────────────────────────
    e2_memset(buf, 0, sizeof(buf));
    Ext2GroupDesc* gd = (Ext2GroupDesc*)buf;

    gd->bg_block_bitmap     = BLK_BMAP;
    gd->bg_inode_bitmap     = BLK_IMAP;
    gd->bg_inode_table      = BLK_ITABLE;
    gd->bg_free_blocks_count= (uint16_t)FREE_BLOCKS;
    gd->bg_free_inodes_count= (uint16_t)(INODES_PER_GROUP - 11u);
    gd->bg_used_dirs_count  = 1;   // root dizin

    // GDT = block 2 → LBA 4-5
    uint32_t gdt_lba = BLK_GDT * SECTORS_PER_BLOCK;
    if (!disk_write_sector64(gdt_lba,     buf))       return -1;
    if (!disk_write_sector64(gdt_lba + 1, buf + 512)) return -1;

    // ── 3. Block bitmap ──────────────────────────────────────
    e2_memset(buf, 0, sizeof(buf));
    // İşaretli (kullanılmış) bloklar: block 1..BLK_DATA + root dir block (BLK_DATA)
    // Toplam META_BLOCKS+1 block = BLK_DATA+1 - FIRST_DATA_BLOCK = 133 block
    uint32_t used = META_BLOCKS + 1u;  // +1 = root dizin data block
    for (uint32_t b = 0; b < used; b++)
        buf[b / 8] |= (uint8_t)(1u << (b % 8));

    uint32_t bmap_lba = BLK_BMAP * SECTORS_PER_BLOCK;
    if (!disk_write_sector64(bmap_lba,     buf))       return -1;
    if (!disk_write_sector64(bmap_lba + 1, buf + 512)) return -1;

    // ── 4. Inode bitmap ──────────────────────────────────────
    e2_memset(buf, 0, sizeof(buf));
    // Inode 1-11 rezerve (sistem inode'ları); inode 2 = root (kullanımda)
    // Bit 0 = inode 1, bit 1 = inode 2, ...
    for (uint32_t i = 0; i < 11u; i++)
        buf[i / 8] |= (uint8_t)(1u << (i % 8));

    uint32_t imap_lba = BLK_IMAP * SECTORS_PER_BLOCK;
    if (!disk_write_sector64(imap_lba,     buf))       return -1;
    if (!disk_write_sector64(imap_lba + 1, buf + 512)) return -1;

    // ── 5. Inode table — hepsini sıfırla, sonra root'u yaz ──
    uint32_t itable_lba = BLK_ITABLE * SECTORS_PER_BLOCK;
    e2_memset(buf, 0, sizeof(buf));
    for (uint32_t s = 0; s < INODE_TABLE_BLOCKS * SECTORS_PER_BLOCK; s++)
        disk_write_sector64(itable_lba + s, buf);

    // Root inode (inode 2): inode tablosunun başından 1*INODE_SIZE offset
    // (inode 1 = bad blocks inode, inode 2 = root)
    e2_memset(buf, 0, sizeof(buf));
    Ext2Inode* root_ino = (Ext2Inode*)buf;
    root_ino->i_mode        = EXT2_S_IFDIR | 0755u;
    root_ino->i_links_count = 2;          // "." + ".."
    root_ino->i_size        = BLOCK_SIZE; // bir block dolu
    root_ino->i_blocks      = SECTORS_PER_BLOCK;  // 512-byte biriminde
    root_ino->i_block[0]    = BLK_DATA;   // root dizinin data block'u

    // Root inode table içinde byte offset = 1 * INODE_SIZE = 128
    // LBA = itable_lba + (128 / 512) = itable_lba + 0  (aynı sektör)
    // Byte offset sektör içinde = 128
    static uint8_t itable_sec[512];
    e2_memset(itable_sec, 0, 512);
    e2_memcpy(itable_sec + INODE_SIZE, root_ino, sizeof(Ext2Inode));  // inode 2 = offset 128
    if (!disk_write_sector64(itable_lba, itable_sec)) return -1;

    // ── 6. Root dizin data block ─────────────────────────────
    // Ext2 dizin entry'leri: "." ve ".."
    e2_memset(buf, 0, sizeof(buf));
    Ext2DirEntry* de;

    // "." entry
    de = (Ext2DirEntry*)buf;
    de->inode     = EXT2_ROOT_INO;   // 2
    de->rec_len   = 12;              // 8 + 1 (name) + 3 (pad)
    de->name_len  = 1;
    de->file_type = EXT2_FT_DIR;
    de->name[0]   = '.';

    // ".." entry
    de = (Ext2DirEntry*)(buf + 12);
    de->inode     = EXT2_ROOT_INO;   // root'un parent'ı kendisi
    de->rec_len   = (uint16_t)(BLOCK_SIZE - 12u);  // bloğun geri kalanı
    de->name_len  = 2;
    de->file_type = EXT2_FT_DIR;
    de->name[0]   = '.';
    de->name[1]   = '.';

    uint32_t data_lba = BLK_DATA * SECTORS_PER_BLOCK;
    if (!disk_write_sector64(data_lba,     buf))       return -1;
    if (!disk_write_sector64(data_lba + 1, buf + 512)) return -1;

    serial_print("[EXT2] Format OK\n");
    return 0;
}

// ============================================================
//  ext2_mount
// ============================================================
int ext2_mount(void) {
    // Zaten mount edilmişse tekrar yapma — state'i bozma
    if (state.mounted) return 0;

    // Superblock: LBA 2 (byte offset 1024)
    static uint8_t sb_buf[EXT2_SECTOR_SIZE * 2];  // 2 sektör = 1024 byte
    if (!disk_read_sector64(EXT2_SUPERBLOCK_LBA,     sb_buf))       return -1;
    if (!disk_read_sector64(EXT2_SUPERBLOCK_LBA + 1, sb_buf + 512)) return -1;

    Ext2Superblock* sb = (Ext2Superblock*)sb_buf;

    if (sb->s_magic != EXT2_SUPER_MAGIC) return -1;

    state.block_size       = 1024u << sb->s_log_block_size;
    state.sectors_per_block= state.block_size / EXT2_SECTOR_SIZE;
    state.inodes_per_group = sb->s_inodes_per_group;
    state.blocks_per_group = sb->s_blocks_per_group;
    state.first_data_block = sb->s_first_data_block;
    state.inodes_count     = sb->s_inodes_count;
    state.blocks_count     = sb->s_blocks_count;

    if (sb->s_rev_level >= 1) {
        state.inode_size = sb->s_inode_size;
        state.first_ino  = sb->s_first_ino;
    } else {
        state.inode_size = EXT2_INODE_SIZE_REV0;
        state.first_ino  = 11;
    }

    // Grup sayısı
    state.num_groups = (sb->s_blocks_count + sb->s_blocks_per_group - 1)
                        / sb->s_blocks_per_group;
    if (state.num_groups == 0) return -1;
    if (state.num_groups > 64) state.num_groups = 64;  // statik GDT sınırı

    state.cwd[0] = '/';
    state.cwd[1] = '\0';
    state.mounted = 1;
    gdt_loaded = 0;  // İlk mount'ta GDT cache'i temizle
    ext2_init_clock();  // Timestamp için boot zamanını kaydet

    serial_print("[EXT2] Mounted OK\n");
    return 0;
}

int ext2_unmount(void) {
    state.mounted = 0;
    gdt_loaded    = 0;
    return 0;
}

// ============================================================
//  ext2_path_is_file / ext2_path_is_dir
// ============================================================
int ext2_path_is_file(const char* path) {
    if (!state.mounted || !path) return 0;
    uint32_t ino = path_resolve(path, 0, NULL);
    if (!ino) return 0;
    Ext2Inode inode;
    if (!read_inode(ino, &inode)) return 0;
    return ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFREG) ? 1 : 0;
}

int ext2_path_is_dir(const char* path) {
    if (!state.mounted || !path) return 0;
    uint32_t ino = path_resolve(path, 0, NULL);
    if (!ino) return 0;
    Ext2Inode inode;
    if (!read_inode(ino, &inode)) return 0;
    return ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) ? 1 : 0;
}

// ============================================================
//  ext2_file_size
// ============================================================
uint32_t ext2_file_size(const char* path) {
    if (!state.mounted || !path) return 0;
    uint32_t ino = path_resolve(path, 0, NULL);
    if (!ino) return 0;
    Ext2Inode inode;
    if (!read_inode(ino, &inode)) return 0;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return 0;
    return inode.i_size;
}

// ============================================================
//  ext2_read_file
// ============================================================
int ext2_read_file(const char* path, uint8_t* buf, uint32_t max_len) {
    if (!state.mounted || !path || !buf) return -1;

    uint32_t ino = path_resolve(path, 0, NULL);
    if (!ino) return -1;

    Ext2Inode inode;
    if (!read_inode(ino, &inode)) return -1;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return -1;

    uint32_t fsize   = inode.i_size;
    uint32_t to_read = (fsize < max_len) ? fsize : max_len;
    uint32_t done    = 0;

    static uint8_t fblk[MAX_BLOCK_SIZE];

    while (done < to_read) {
        uint32_t blk_idx   = done / state.block_size;
        uint32_t intra_off = done % state.block_size;
        uint32_t phys      = read_file_block(&inode, blk_idx);

        uint32_t chunk = state.block_size - intra_off;
        if (done + chunk > to_read) chunk = to_read - done;

        if (!phys) {
            // Sparse blok — ext2 standardı: sıfır byte, break etme
            e2_memset(buf + done, 0, chunk);
        } else {
            if (!read_block(phys, fblk)) break;
            e2_memcpy(buf + done, fblk + intra_off, chunk);
        }
        done += chunk;
    }

    return (int)done;
}

// ============================================================
//  ext2_write_file — offset tabanlı, block alloc dahil
// ============================================================
int ext2_write_file(const char* path, uint64_t offset,
                     const uint8_t* data, uint32_t len) {
    if (!state.mounted || !path || !data || len == 0) return -1;

    uint32_t ino = path_resolve(path, 0, NULL);
    if (!ino) return -1;

    Ext2Inode inode;
    if (!read_inode(ino, &inode)) return -1;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return -1;

    uint32_t start   = (uint32_t)offset;
    uint32_t written = 0;
    static uint8_t fblk[MAX_BLOCK_SIZE];

    while (written < len) {
        uint32_t abs_off   = start + written;
        uint32_t blk_idx   = abs_off / state.block_size;
        uint32_t intra_off = abs_off % state.block_size;

        uint32_t phys = read_file_block(&inode, blk_idx);
        if (!phys) {
            // Yeni blok tahsis et
            phys = alloc_file_block(&inode, blk_idx);
            if (!phys) goto write_done;
            e2_memset(fblk, 0, state.block_size);
        } else {
            if (!read_block(phys, fblk)) goto write_done;
        }

        uint32_t chunk = state.block_size - intra_off;
        if (written + chunk > len) chunk = len - written;

        e2_memcpy(fblk + intra_off, data + written, chunk);
        if (!write_block(phys, fblk)) goto write_done;

        written += chunk;
    }

write_done:
    // i_size ve i_mtime güncelle
    uint32_t new_end = start + written;
    if (new_end > inode.i_size) {
        inode.i_size = new_end;
        inode.i_blocks = ((new_end + state.block_size - 1) / state.block_size)
                          * state.sectors_per_block * 1;
    }
    inode.i_mtime = ext2_now();
    write_inode(ino, &inode);
    return (int)written;
}

// ============================================================
//  ext2_create_file
// ============================================================
int ext2_create_file(const char* path) {
    if (!state.mounted || !path) return -1;

    char leaf[EXT2_MAX_NAME + 1];
    uint32_t parent_ino = path_resolve(path, 1, leaf);
    if (!parent_ino || leaf[0] == '\0') return -1;

    // Zaten var mı?
    if (dir_lookup(parent_ino, leaf)) return -1;

    uint32_t new_ino = alloc_inode();
    if (!new_ino) return -1;

    Ext2Inode inode;
    e2_memset(&inode, 0, sizeof(inode));
    inode.i_mode        = EXT2_S_IFREG | 0644u;
    inode.i_links_count = 1;
    inode.i_ctime       = ext2_now();
    inode.i_mtime       = inode.i_ctime;
    inode.i_atime       = inode.i_ctime;

    if (!write_inode(new_ino, &inode)) { free_inode(new_ino); return -1; }
    if (dir_add_entry(parent_ino, leaf, new_ino, EXT2_FT_REG_FILE) != 0) {
        free_inode(new_ino); return -1;
    }
    return 0;
}

// ============================================================
//  ext2_mkdir
// ============================================================
int ext2_mkdir(const char* path) {
    if (!state.mounted || !path) return -1;

    char leaf[EXT2_MAX_NAME + 1];
    uint32_t parent_ino = path_resolve(path, 1, leaf);
    if (!parent_ino || leaf[0] == '\0') return -1;

    if (dir_lookup(parent_ino, leaf)) return -1;  // zaten var

    uint32_t new_ino = alloc_inode();
    if (!new_ino) return -1;

    Ext2Inode inode;
    e2_memset(&inode, 0, sizeof(inode));
    inode.i_mode        = EXT2_S_IFDIR | 0755u;
    inode.i_links_count = 2;  // "." + parent'ın bu dizine linki
    inode.i_ctime       = ext2_now();
    inode.i_mtime       = inode.i_ctime;
    inode.i_atime       = inode.i_ctime;

    if (!write_inode(new_ino, &inode)) { free_inode(new_ino); return -1; }

    // "." ve ".." ekle
    if (dir_add_entry(new_ino, ".",  new_ino,    EXT2_FT_DIR) != 0 ||
        dir_add_entry(new_ino, "..", parent_ino, EXT2_FT_DIR) != 0) {
        free_inode(new_ino); return -1;
    }

    // Parent'a yeni dizini ekle
    if (dir_add_entry(parent_ino, leaf, new_ino, EXT2_FT_DIR) != 0) {
        free_inode(new_ino); return -1;
    }

    // Parent'ın links_count'ını artır (her alt dizin parent'a bir ".." linki ekler)
    Ext2Inode par_inode;
    if (read_inode(parent_ino, &par_inode)) {
        par_inode.i_links_count++;
        write_inode(parent_ino, &par_inode);
    }

    // used_dirs_count güncelle
    uint32_t g = (new_ino - 1) / state.inodes_per_group;
    Ext2GroupDesc gd;
    if (read_group_desc(g, &gd)) {
        gd.bg_used_dirs_count++;
        write_group_desc(g, &gd);
    }

    return 0;
}

// ============================================================
//  ext2_mkdir_p — recursive mkdir ("mkdir -p" semantiği)
//  Path boyunca eksik olan her dizini sırayla oluşturur.
//  Zaten var olan bileşenler sessizce atlanır.
//  Döner: 0 başarı, -1 hata
// ============================================================
int ext2_mkdir_p(const char* path) {
    if (!state.mounted || !path || path[0] == '\0') return -1;
    if (path[0] != '/') return -1;  // sadece mutlak path

    char buf[EXT2_MAX_PATH];
    int  len = 0;
    while (path[len] && len < (int)EXT2_MAX_PATH - 1) {
        buf[len] = path[len];
        len++;
    }
    buf[len] = '\0';

    // Path bileşenlerini sırayla işle
    for (int i = 1; i <= len; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';

            // Bu prefixe kadar olan yolu oluştur (root "/" atla)
            if (i > 1) {
                // Zaten var mı?
                if (!ext2_path_is_dir(buf)) {
                    int r = ext2_mkdir(buf);
                    if (r != 0) return -1;  // oluşturulamadı
                }
            }

            buf[i] = saved;
        }
    }
    return 0;
}

// ============================================================
//  ext2_rmdir — sadece boş dizinler
// ============================================================
int ext2_rmdir(const char* path) {
    if (!state.mounted || !path) return -1;

    char leaf[EXT2_MAX_NAME + 1];
    uint32_t parent_ino = path_resolve(path, 1, leaf);
    if (!parent_ino || leaf[0] == '\0') return -1;

    uint32_t dir_ino = dir_lookup(parent_ino, leaf);
    if (!dir_ino) return -1;

    Ext2Inode dir_inode;
    if (!read_inode(dir_ino, &dir_inode)) return -1;
    if ((dir_inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    // Boş mu? (yalnızca "." ve ".." olmalı)
    uint32_t dir_size = dir_inode.i_size;
    uint32_t offset   = 0;
    int      entry_count = 0;
    static uint8_t dblk[MAX_BLOCK_SIZE];

    uint32_t blk_idx = 0;
    while (offset < dir_size) {
        uint32_t phys = read_file_block(&dir_inode, blk_idx++);
        if (!phys) break;
        if (!read_block(phys, dblk)) break;

        uint32_t intra = 0;
        while (intra < state.block_size && offset < dir_size) {
            Ext2DirEntry* de = (Ext2DirEntry*)(dblk + intra);
            if (de->rec_len == 0) break;
            if (de->inode) {
                // "." ve ".." sayılmaz
                if (!(de->name_len == 1 && de->name[0] == '.') &&
                    !(de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.'))
                    entry_count++;
            }
            intra  += de->rec_len;
            offset += de->rec_len;
        }
    }

    if (entry_count > 0) return -1;  // ENOTEMPTY

    // Blokları serbest bırak
    free_inode_blocks(&dir_inode);
    dir_inode.i_dtime = 0;
    dir_inode.i_links_count = 0;
    write_inode(dir_ino, &dir_inode);
    free_inode(dir_ino);

    // Parent'dan entry'yi kaldır
    dir_remove_entry(parent_ino, leaf);

    // Parent links_count azalt
    Ext2Inode par_inode;
    if (read_inode(parent_ino, &par_inode)) {
        if (par_inode.i_links_count > 1) par_inode.i_links_count--;
        write_inode(parent_ino, &par_inode);
    }

    // GDT used_dirs_count azalt
    uint32_t g = (dir_ino - 1) / state.inodes_per_group;
    Ext2GroupDesc gd;
    if (read_group_desc(g, &gd)) {
        if (gd.bg_used_dirs_count > 0) gd.bg_used_dirs_count--;
        write_group_desc(g, &gd);
    }

    return 0;
}

// ============================================================
//  ext2_unlink
// ============================================================
int ext2_unlink(const char* path) {
    if (!state.mounted || !path) return -1;

    char leaf[EXT2_MAX_NAME + 1];
    uint32_t parent_ino = path_resolve(path, 1, leaf);
    if (!parent_ino || leaf[0] == '\0') return -1;

    uint32_t file_ino = dir_lookup(parent_ino, leaf);
    if (!file_ino) return -1;

    Ext2Inode inode;
    if (!read_inode(file_ino, &inode)) return -1;
    if ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return -1;  // EISDIR

    // links_count azalt
    if (inode.i_links_count > 0) inode.i_links_count--;

    if (inode.i_links_count == 0) {
        // Gerçekten sil: blokları ve inode'u serbest bırak
        free_inode_blocks(&inode);
        inode.i_dtime = 0;
        write_inode(file_ino, &inode);
        free_inode(file_ino);
    } else {
        write_inode(file_ino, &inode);
    }

    dir_remove_entry(parent_ino, leaf);
    return 0;
}

// ============================================================
//  ext2_rename
// ============================================================
int ext2_rename(const char* oldpath, const char* newpath) {
    if (!state.mounted || !oldpath || !newpath) return -1;

    char old_leaf[EXT2_MAX_NAME + 1];
    char new_leaf[EXT2_MAX_NAME + 1];
    uint32_t old_parent = path_resolve(oldpath, 1, old_leaf);
    uint32_t new_parent = path_resolve(newpath, 1, new_leaf);
    if (!old_parent || !new_parent) return -1;
    if (old_leaf[0] == '\0' || new_leaf[0] == '\0') return -1;

    uint32_t src_ino = dir_lookup(old_parent, old_leaf);
    if (!src_ino) return -1;

    Ext2Inode src_inode;
    if (!read_inode(src_ino, &src_inode)) return -1;

    int is_dir = ((src_inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);

    // Hedef varsa kaldır (basit overwrite)
    uint32_t dst_ino = dir_lookup(new_parent, new_leaf);
    if (dst_ino && dst_ino != src_ino) {
        Ext2Inode dst_inode;
        if (!read_inode(dst_ino, &dst_inode)) return -1;
        // Dizinse rmdir, değilse unlink
        if ((dst_inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
            // Sadece boşsa
            ext2_rmdir(newpath);
        } else {
            ext2_unlink(newpath);
        }
    }

    // Yeni parent'a entry ekle
    uint8_t ft = is_dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
    if (dir_add_entry(new_parent, new_leaf, src_ino, ft) != 0) return -1;

    // Eski entry'yi kaldır
    dir_remove_entry(old_parent, old_leaf);

    // Eğer dizin ve parent değiştiyse ".." entry'sini güncelle
    if (is_dir && old_parent != new_parent) {
        // ".." entry'sini güncelle
        Ext2Inode nd;
        if (read_inode(src_ino, &nd)) {
            uint32_t dir_size = nd.i_size;
            uint32_t offset   = 0;
            static uint8_t dblk[MAX_BLOCK_SIZE];
            uint32_t bidx    = 0;
            while (offset < dir_size) {
                uint32_t phys = read_file_block(&nd, bidx++);
                if (!phys) break;
                if (!read_block(phys, dblk)) break;
                uint32_t intra = 0;
                int updated = 0;
                while (intra < state.block_size && offset < dir_size) {
                    Ext2DirEntry* de = (Ext2DirEntry*)(dblk + intra);
                    if (de->rec_len == 0) break;
                    if (de->inode && de->name_len == 2 &&
                        de->name[0] == '.' && de->name[1] == '.') {
                        de->inode = new_parent;
                        write_block(phys, dblk);
                        updated = 1;
                        break;
                    }
                    intra  += de->rec_len;
                    offset += de->rec_len;
                }
                if (updated) break;
            }
        }

        // Parent links_count güncelle
        Ext2Inode old_par, new_par;
        if (read_inode(old_parent, &old_par)) {
            if (old_par.i_links_count > 1) old_par.i_links_count--;
            write_inode(old_parent, &old_par);
        }
        if (read_inode(new_parent, &new_par)) {
            new_par.i_links_count++;
            write_inode(new_parent, &new_par);
        }
    }

    return 0;
}

// ============================================================
//  ext2_truncate
// ============================================================
int ext2_truncate(const char* path, uint64_t length) {
    if (!state.mounted || !path) return -1;

    uint32_t ino = path_resolve(path, 0, NULL);
    if (!ino) return -1;

    Ext2Inode inode;
    if (!read_inode(ino, &inode)) return -1;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFREG) return -1;

    uint32_t new_size = (uint32_t)length;
    uint32_t old_size = inode.i_size;

    if (new_size == old_size) return 0;

    if (new_size < old_size) {
        // Kırp: new_size'dan sonraki blokları serbest bırak
        uint32_t old_blocks = (old_size + state.block_size - 1) / state.block_size;
        uint32_t new_blocks = (new_size + state.block_size - 1) / state.block_size;

        for (uint32_t i = new_blocks; i < old_blocks; i++) {
            uint32_t phys = read_file_block(&inode, i);
            if (phys) free_block(phys);
            // i_block pointer'larını temizleme: basitlik için sadece direct
            if (i < 12) inode.i_block[i] = 0;
        }

        // Son bloğun artık kısmını sıfırla (kısmi blok)
        if (new_size > 0 && (new_size % state.block_size) != 0) {
            uint32_t last_blk   = (new_size - 1) / state.block_size;
            uint32_t last_phys  = read_file_block(&inode, last_blk);
            if (last_phys) {
                static uint8_t fblk[MAX_BLOCK_SIZE];
                if (read_block(last_phys, fblk)) {
                    uint32_t tail = new_size % state.block_size;
                    e2_memset(fblk + tail, 0, state.block_size - tail);
                    write_block(last_phys, fblk);
                }
            }
        }
    } else {
        // Genişlet: sıfırla doldur
        static uint8_t zero[MAX_BLOCK_SIZE];
        e2_memset(zero, 0, state.block_size);
        uint32_t old_blocks = (old_size + state.block_size - 1) / state.block_size;
        uint32_t new_blocks = (new_size + state.block_size - 1) / state.block_size;

        for (uint32_t i = old_blocks; i < new_blocks; i++) {
            uint32_t phys = alloc_file_block(&inode, i);
            if (!phys) break;
            write_block(phys, zero);
        }
    }

    inode.i_size = new_size;
    write_inode(ino, &inode);
    return 0;
}

// ============================================================
//  ext2_getdents
// ============================================================
int ext2_getdents(const char* path, dirent64_t* buf, int buf_size) {
    if (!state.mounted || !path || !buf || buf_size <= 0) return -1;

    uint32_t dir_ino = path_resolve(path, 0, NULL);
    if (!dir_ino) return -1;

    Ext2Inode dir_inode;
    if (!read_inode(dir_ino, &dir_inode)) return -1;
    if ((dir_inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    uint32_t dir_size = dir_inode.i_size;
    uint32_t offset   = 0;
    int      total    = 0;
    uint64_t ino_ctr  = 1;

    static uint8_t dblk[MAX_BLOCK_SIZE];

#define WDIRENT(nm, nmlen, tp) do {                                     \
    int reclen_ = (int)((19 + (nmlen) + 1 + 7) & ~7u);                 \
    if (total + reclen_ > buf_size) goto gd_done;                       \
    dirent64_t* de_ = (dirent64_t*)((char*)buf + total);               \
    de_->d_ino    = ino_ctr++;                                          \
    de_->d_off    = (uint64_t)(total + reclen_);                        \
    de_->d_reclen = (uint16_t)reclen_;                                  \
    de_->d_type   = (tp);                                               \
    for (int _i = 0; _i < (nmlen); _i++) de_->d_name[_i] = (nm)[_i];  \
    de_->d_name[(nmlen)] = '\0';                                        \
    total += reclen_;                                                    \
} while(0)

    uint32_t blk_idx = 0;
    while (offset < dir_size) {
        uint32_t phys = read_file_block(&dir_inode, blk_idx++);
        if (!phys) break;
        if (!read_block(phys, dblk)) break;

        uint32_t intra = 0;
        while (intra < state.block_size && offset < dir_size) {
            Ext2DirEntry* de = (Ext2DirEntry*)(dblk + intra);
            if (de->rec_len == 0) break;

            if (de->inode && de->name_len > 0) {
                uint8_t dtype;
                if (de->file_type == EXT2_FT_DIR)      dtype = DT_DIR;
                else if (de->file_type == EXT2_FT_REG_FILE) dtype = DT_REG;
                else {
                    // file_type yoksa inode'dan kontrol et
                    Ext2Inode ti;
                    dtype = DT_UNKNOWN;
                    if (read_inode(de->inode, &ti)) {
                        uint16_t m = ti.i_mode & EXT2_S_IFMT;
                        if (m == EXT2_S_IFDIR) dtype = DT_DIR;
                        else if (m == EXT2_S_IFREG) dtype = DT_REG;
                    }
                }
                WDIRENT(de->name, de->name_len, dtype);
            }

            intra  += de->rec_len;
            offset += de->rec_len;
        }
    }

#undef WDIRENT

gd_done:
    return total;
}

// ============================================================
//  CWD: ext2_getcwd / ext2_chdir
// ============================================================
const char* ext2_getcwd(void) {
    return state.cwd;
}

int ext2_chdir(const char* path) {
    if (!state.mounted || !path) return -1;

    // ".." özel durumu
    if (e2_strcmp(path, "..") == 0) {
        if (state.cwd[0] == '/' && state.cwd[1] == '\0') return 0;
        int len = e2_strlen(state.cwd);
        while (len > 1 && state.cwd[len - 1] != '/') len--;
        if (len > 1) len--;  // '/' karakterini de sil (root hariç)
        state.cwd[len] = '\0';
        if (state.cwd[0] == '\0') { state.cwd[0] = '/'; state.cwd[1] = '\0'; }
        return 0;
    }

    if (e2_strcmp(path, ".") == 0) return 0;

    // Çöz ve doğrula
    uint32_t ino = path_resolve(path, 0, NULL);
    if (!ino) return -1;

    Ext2Inode inode;
    if (!read_inode(ino, &inode)) return -1;
    if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    // CWD'yi güncelle
    if (path[0] == '/') {
        e2_strcpy(state.cwd, path);
    } else {
        int cwdlen = e2_strlen(state.cwd);
        if (cwdlen > 1) { state.cwd[cwdlen] = '/'; state.cwd[cwdlen + 1] = '\0'; }
        e2_strcat(state.cwd, path);
    }

    // Sondaki '/' temizle
    int len = e2_strlen(state.cwd);
    if (len > 1 && state.cwd[len - 1] == '/') state.cwd[len - 1] = '\0';

    return 0;
}