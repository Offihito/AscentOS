#include "fs/ext3.h"
#include "console/klog.h"
#include "mm/heap.h"
#include "lib/string.h"
#include <stdbool.h>

static int ext3_recover_journal(ext2_mount_t *mnt, jbd_superblock_t *jsb, ext2_inode_t *j_inode);

// Internal state for the current active transaction
static struct {
    bool active;
    uint32_t sequence;
    uint32_t start_block; // Current write position in journal
    ext2_inode_t journal_inode;
    uint8_t *desc_block_buf;
    uint32_t blocks_in_trans;
} current_trans = {0};

void ext3_init_journal(ext2_mount_t *mnt) {
    if (!(mnt->sb.s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL)) {
        return; // No journal
    }

    uint32_t journal_ino = mnt->sb.s_journal_inum;
    if (journal_ino == 0) {
        klog_puts("[EXT3] Journal feature enabled but journal inode is 0\n");
        return;
    }

    if (ext2_read_inode(mnt, journal_ino, &current_trans.journal_inode) != 0) {
        klog_puts("[EXT3] Failed to read journal inode\n");
        return;
    }

    uint32_t disk_block = ext2_get_block_num(mnt, &current_trans.journal_inode, 0);
    if (!disk_block) {
        klog_puts("[EXT3] Journal inode has no blocks\n");
        return;
    }

    uint8_t *buf = kmalloc(mnt->block_size);
    if (!buf) {
        klog_puts("[EXT3] Out of memory for journal read\n");
        return;
    }

    if (ext2_read_block(mnt, disk_block, buf) != 0) {
        klog_puts("[EXT3] Failed to read journal block\n");
        kfree(buf);
        return;
    }

    jbd_superblock_t *jsb = (jbd_superblock_t *)buf;

    uint32_t magic = __builtin_bswap32(jsb->s_header.h_magic);
    if (magic != EXT3_JOURNAL_MAGIC_NUMBER) {
        klog_puts("[EXT3] Invalid journal magic\n");
        kfree(buf);
        return;
    }

    current_trans.sequence = __builtin_bswap32(jsb->s_sequence);
    uint32_t start = __builtin_bswap32(jsb->s_start);

    klog_puts("[EXT3] Found valid JBD journal!\n");
    klog_puts("       Sequence:  "); klog_uint64(current_trans.sequence); klog_puts("\n");
    klog_puts("       Start:     "); klog_uint64(start); klog_puts("\n");

    if (start != 0) {
        klog_puts("[EXT3] Journal requires recovery!\n");
        ext3_recover_journal(mnt, jsb, &current_trans.journal_inode);
    } else {
        klog_puts("[EXT3] Journal is clean.\n");
    }

    kfree(buf);
}

int ext3_journal_start(ext2_mount_t *mnt) {
    if (!(mnt->sb.s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL)) return 0;
    if (current_trans.active) return -1;

    current_trans.active = true;
    current_trans.blocks_in_trans = 0;
    
    // Simple impl: start at block 1 of journal for each txn
    current_trans.start_block = 1; 

    // Setup descriptor block buffer
    if (!current_trans.desc_block_buf) current_trans.desc_block_buf = kmalloc(mnt->block_size);
    memset(current_trans.desc_block_buf, 0, mnt->block_size);
    
    jbd_header_t *h = (jbd_header_t *)current_trans.desc_block_buf;
    h->h_magic = __builtin_bswap32(EXT3_JOURNAL_MAGIC_NUMBER);
    h->h_blocktype = __builtin_bswap32(JBD_DESCRIPTOR_BLOCK);
    h->h_sequence = __builtin_bswap32(current_trans.sequence);

    return 0;
}

int ext3_journal_block(ext2_mount_t *mnt, uint32_t block_nr, const void *data) {
    if (!current_trans.active) return ext2_write_block(mnt, block_nr, data);

    // 1. Add tag to descriptor block
    uint32_t tag_offset = sizeof(jbd_header_t) + (current_trans.blocks_in_trans * sizeof(jbd_block_tag_t));
    if (tag_offset + sizeof(jbd_block_tag_t) > mnt->block_size) {
        return -1; 
    }

    jbd_block_tag_t *tag = (jbd_block_tag_t *)(current_trans.desc_block_buf + tag_offset);
    tag->t_blocknr = __builtin_bswap32(block_nr);
    tag->t_flags = 0;

    // 2. Write data block to journal
    uint32_t journal_data_pos = current_trans.start_block + 1 + current_trans.blocks_in_trans;
    uint32_t phys_pos = ext2_get_block_num(mnt, &current_trans.journal_inode, journal_data_pos);
    
    // Handle Escaping
    uint8_t *safe_data = (uint8_t *)data;
    if (*(const uint32_t *)data == __builtin_bswap32(EXT3_JOURNAL_MAGIC_NUMBER)) {
        tag->t_flags |= __builtin_bswap32(JBD_FLAG_ESCAPE);
    }

    ext2_write_block(mnt, phys_pos, safe_data);

    current_trans.blocks_in_trans++;
    return 0;
}

int ext3_journal_stop(ext2_mount_t *mnt) {
    if (!current_trans.active) return 0;

    // 1. Mark last tag
    if (current_trans.blocks_in_trans > 0) {
        uint32_t last_tag_off = sizeof(jbd_header_t) + ((current_trans.blocks_in_trans - 1) * sizeof(jbd_block_tag_t));
        jbd_block_tag_t *tag = (jbd_block_tag_t *)(current_trans.desc_block_buf + last_tag_off);
        tag->t_flags |= __builtin_bswap32(JBD_FLAG_LAST_TAG);
    }

    // 2. Write descriptor block
    uint32_t desc_phys = ext2_get_block_num(mnt, &current_trans.journal_inode, current_trans.start_block);
    ext2_write_block(mnt, desc_phys, current_trans.desc_block_buf);

    // 3. Write commit block
    uint8_t *commit_buf = kmalloc(mnt->block_size);
    memset(commit_buf, 0, mnt->block_size);
    jbd_header_t *ch = (jbd_header_t *)commit_buf;
    ch->h_magic = __builtin_bswap32(EXT3_JOURNAL_MAGIC_NUMBER);
    ch->h_blocktype = __builtin_bswap32(JBD_COMMIT_BLOCK);
    ch->h_sequence = __builtin_bswap32(current_trans.sequence);
    
    uint32_t commit_pos = current_trans.start_block + 1 + current_trans.blocks_in_trans;
    uint32_t commit_phys = ext2_get_block_num(mnt, &current_trans.journal_inode, commit_pos);
    ext2_write_block(mnt, commit_phys, commit_buf);
    kfree(commit_buf);

    // 4. Update journal superblock to mark "Start" (simulating a crash window here)
    uint8_t *sb_buf = kmalloc(mnt->block_size);
    uint32_t sb_phys = ext2_get_block_num(mnt, &current_trans.journal_inode, 0);
    ext2_read_block(mnt, sb_phys, sb_buf);
    jbd_superblock_t *jsb = (jbd_superblock_t *)sb_buf;
    jsb->s_start = __builtin_bswap32(current_trans.start_block);
    ext2_write_block(mnt, sb_phys, sb_buf);

    // 5. Commit to real file system
    for (uint32_t i = 0; i < current_trans.blocks_in_trans; i++) {
        uint32_t tag_off = sizeof(jbd_header_t) + (i * sizeof(jbd_block_tag_t));
        jbd_block_tag_t *tag = (jbd_block_tag_t *)(current_trans.desc_block_buf + tag_off);
        uint32_t target_nr = __builtin_bswap32(tag->t_blocknr);
        
        uint32_t j_data_pos = current_trans.start_block + 1 + i;
        uint32_t j_phys = ext2_get_block_num(mnt, &current_trans.journal_inode, j_data_pos);
        
        uint8_t *tmp = kmalloc(mnt->block_size);
        ext2_read_block(mnt, j_phys, tmp);
        ext2_write_block(mnt, target_nr, tmp);
        kfree(tmp);
    }

    current_trans.sequence++;
    current_trans.active = false;
    
    // 6. Finalize journal superblock (mark clean)
    jsb->s_sequence = __builtin_bswap32(current_trans.sequence);
    jsb->s_start = 0; 
    ext2_write_block(mnt, sb_phys, sb_buf);
    kfree(sb_buf);

    return 0;
}

static int ext3_recover_journal(ext2_mount_t *mnt, jbd_superblock_t *jsb, ext2_inode_t *j_inode) {
    uint32_t block_size = mnt->block_size;
    uint32_t start_block = __builtin_bswap32(jsb->s_start);
    uint32_t sequence = __builtin_bswap32(jsb->s_sequence);
    uint32_t journal_blocks = __builtin_bswap32(jsb->s_maxlen);
    
    klog_puts("[EXT3] Starting journal recovery...\n");

    uint8_t *desc_buf = kmalloc(block_size);
    uint8_t *data_buf = kmalloc(block_size);
    if (!desc_buf || !data_buf) return -1;

    uint32_t curr_journal_block = start_block;
    while (1) {
        uint32_t phys_block = ext2_get_block_num(mnt, j_inode, curr_journal_block);
        if (ext2_read_block(mnt, phys_block, desc_buf) != 0) break;

        jbd_header_t *header = (jbd_header_t *)desc_buf;
        if (__builtin_bswap32(header->h_magic) != EXT3_JOURNAL_MAGIC_NUMBER) break;
        if (__builtin_bswap32(header->h_sequence) != sequence) break;

        uint32_t type = __builtin_bswap32(header->h_blocktype);
        if (type == JBD_DESCRIPTOR_BLOCK) {
            uint32_t tag_offset = sizeof(jbd_header_t);
            while (tag_offset < block_size) {
                jbd_block_tag_t *tag = (jbd_block_tag_t *)(desc_buf + tag_offset);
                uint32_t target_block = __builtin_bswap32(tag->t_blocknr);
                uint32_t flags = __builtin_bswap32(tag->t_flags);
                
                curr_journal_block = (curr_journal_block % (journal_blocks - 1)) + 1;
                uint32_t data_phys = ext2_get_block_num(mnt, j_inode, curr_journal_block);
                ext2_read_block(mnt, data_phys, data_buf);
                
                if (__builtin_bswap32(flags) & JBD_FLAG_ESCAPE) {
                   uint32_t magic = __builtin_bswap32(EXT3_JOURNAL_MAGIC_NUMBER);
                   memcpy(data_buf, &magic, 4);
                }

                klog_puts("       Recovering block: "); klog_uint64(target_block); klog_puts("\n");
                ext2_write_block(mnt, target_block, data_buf);

                if (__builtin_bswap32(flags) & JBD_FLAG_LAST_TAG) break;
                tag_offset += sizeof(jbd_block_tag_t);
            }
        } else if (type == JBD_COMMIT_BLOCK) {
            sequence++;
        } else {
            break; 
        }

        curr_journal_block = (curr_journal_block % (journal_blocks - 1)) + 1;
        if (curr_journal_block == start_block) break;
    }

    klog_puts("[EXT3] Journal recovery complete.\n");
    kfree(desc_buf);
    kfree(data_buf);
    return 0;
}
