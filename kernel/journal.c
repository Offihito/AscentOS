#include "journal.h"
#include "ext3.h"
#include "heap.h"

extern void* memset64(void* dest, int val, uint64_t n);
extern void* memcpy64(void* dest, const void* src, uint64_t n);

#define kmemset(d, v, n) memset64((d), (v), (uint64_t)(n))
#define kmemcpy(d, s, n) memcpy64((d), (s), (uint64_t)(n))

static JournalState journal_state = {0};
static uint32_t next_handle_id = 1;

uint32_t journal_compute_crc32(const uint8_t* data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    static const uint32_t poly = 0xEDB88320u;
    
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ poly;
            } else {
                crc >>= 1;
            }
        }
    }
    
    return crc ^ 0xFFFFFFFFu;
}

int journal_init(void) {
    if (journal_state.running) {
        return -1;
    }
    
    const Ext3State* fs = ext3_get_state();
    if (!fs || !fs->mounted) {
        return -2;
    }
    
    journal_state.journal_ino = EXT3_JOURNAL_INO;
    journal_state.journal_blocksize = fs->block_size;
    journal_state.state = JOURNAL_STATE_RUNNING;
    journal_state.h_sequence = 1;
    journal_state.h_start = 0;
    journal_state.h_tail = 0;
    journal_state.running = 1;
    journal_state.lock = 0;
    
    // Do NOT allocate a large transaction buffer here - the journal is a lightweight
    // stub and all operations are no-ops. A 4MB kmalloc exhausts the kernel heap
    // and leaves nothing for task structures / ELF loading. The buffer fields are
    // left zeroed (NULL / 0) which is safe because no stub function reads them.
    journal_state.transaction_buffer = NULL;
    journal_state.trans_buf_size = 0;
    journal_state.trans_buf_used = 0;
    
    return 0;
}

int journal_destroy(void) {
    if (!journal_state.running) {
        return -1;
    }
    
    journal_state.running = 0;
    journal_state.state = JOURNAL_STATE_EMPTY;
    
    if (journal_state.transaction_buffer) {
        kfree(journal_state.transaction_buffer);
        journal_state.transaction_buffer = NULL;
    }
    journal_state.trans_buf_size = 0;
    journal_state.trans_buf_used = 0;
    
    return 0;
}

int journal_format(void) {
    if (!journal_state.running) {
        return -1;
    }
    
    return 0;
}

JournalHandle* journal_start(void) {
    if (!journal_state.running || journal_state.state != JOURNAL_STATE_RUNNING) {
        return NULL;
    }
    
    JournalHandle* h = (JournalHandle*)kmalloc(sizeof(JournalHandle));
    if (!h) {
        return NULL;
    }
    
    h->handle_id = next_handle_id++;
    h->num_modified = 0;
    h->modified_blocks = (uint32_t*)kmalloc(256 * sizeof(uint32_t));
    
    if (!h->modified_blocks) {
        kfree(h);
        return NULL;
    }
    
    h->num_buffers = 256;  // track allocated capacity for grow logic
    kmemset(h->modified_blocks, 0, 256 * sizeof(uint32_t));
    
    return h;
}

int journal_stop(JournalHandle* h) {
    if (!h) {
        return -1;
    }
    
    journal_state.h_sequence++;
    
    if (h->modified_blocks) {
        kfree(h->modified_blocks);
    }
    kfree(h);
    
    return 0;
}

int journal_abort(JournalHandle* h) {
    if (!h) {
        return -1;
    }
    
    // Do NOT set global state to ABORTED - that permanently breaks journal_start()
    // for all future transactions. Only free this handle.
    if (h->modified_blocks) {
        kfree(h->modified_blocks);
        h->modified_blocks = NULL;
    }
    kfree(h);
    
    return 0;
}

int journal_get_write_access(JournalHandle* h, uint32_t block_num) {
    if (!h || !h->modified_blocks) {
        return -1;
    }
    
    if (h->num_modified >= h->num_buffers) {
        uint32_t new_cap = h->num_buffers * 2;
        uint32_t* new_blocks = (uint32_t*)krealloc(h->modified_blocks,
                                                    new_cap * sizeof(uint32_t));
        if (!new_blocks) {
            return -2;
        }
        h->modified_blocks = new_blocks;
        h->num_buffers = new_cap;
    }
    
    h->modified_blocks[h->num_modified++] = block_num;
    return 0;
}

int journal_dirty_metadata(JournalHandle* h, uint32_t block_num) {
    if (!h || !h->modified_blocks) {
        return -1;
    }
    
    if (h->num_modified >= h->num_buffers) {
        uint32_t new_cap = h->num_buffers * 2;
        uint32_t* new_blocks = (uint32_t*)krealloc(h->modified_blocks,
                                                    new_cap * sizeof(uint32_t));
        if (!new_blocks) {
            return -2;
        }
        h->modified_blocks = new_blocks;
        h->num_buffers = new_cap;
    }
    
    h->modified_blocks[h->num_modified++] = block_num;
    return 0;
}

int journal_extend(JournalHandle* h, uint32_t size) {
    if (!h) {
        return -1;
    }
    // transaction_buffer is a stub - nothing to extend
    (void)size;
    return 0;
}

int journal_verify_checksum(const uint8_t* data, uint32_t len, uint32_t stored_crc) {
    uint32_t computed_crc = journal_compute_crc32(data, len);
    return (computed_crc == stored_crc) ? 0 : -1;
}

int journal_recover(void) {
    if (!journal_state.running) {
        return -1;
    }
    
    return 0;
}

int journal_replay_transaction(uint32_t sequence) {
    (void)sequence;
    
    if (!journal_state.running) {
        return -1;
    }
    
    return 0;
}

int journal_add_orphan(JournalHandle* h, uint32_t inode_num) {
    (void)h;
    (void)inode_num;
    
    if (!journal_state.running) {
        return -1;
    }
    
    return 0;
}

int journal_remove_orphan(JournalHandle* h, uint32_t inode_num) {
    (void)h;
    (void)inode_num;
    
    if (!journal_state.running) {
        return -1;
    }
    
    return 0;
}

JournalState* journal_get_state(void) {
    return &journal_state;
}