#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdint.h>

#define EXT3_JOURNAL_INO        8u
#define EXT3_JOURNAL_SUPERBLOCK_OFFSET 0
#define JOURNAL_MAGIC           0xc03b3998u

#define JBD_DESCRIPTOR_BLOCK    1
#define JBD_COMMIT_BLOCK        2
#define JBD_SUPERBLOCK_BLOCK    3
#define JBD_REVOKE_BLOCK        4

#define JOURNAL_STATE_EMPTY     0
#define JOURNAL_STATE_RUNNING   1
#define JOURNAL_STATE_FLUSHING  2
#define JOURNAL_STATE_COMMITTING 3
#define JOURNAL_STATE_ABORTED   4

#define JBD_FLAG_ESCAPE         1
#define JBD_FLAG_SAME_UUID      2
#define JBD_FLAG_DELETED        4
#define JBD_FLAG_LAST_TAG       8

typedef struct __attribute__((packed)) {
    uint32_t t_blocknr;
    uint32_t t_flags;
    uint32_t t_checksum;
} JbdBlockTag;

typedef struct __attribute__((packed)) {
    uint32_t s_header_magic;
    uint32_t s_blocksize;
    uint32_t s_maxlen;
    uint32_t s_first;
    uint32_t s_sequence;
    uint32_t s_start;
    uint32_t s_errno;
    struct {
        uint32_t u_magic;
        uint8_t  u_uuid[16];
        uint32_t u_start;
        uint32_t u_len;
    } s_users[48];
    uint32_t s_checksum_type;
    uint32_t s_checksum_size;
    uint32_t s_checksum_offset;
    uint8_t  s_reserved[88];
} __attribute__((packed)) JbdSuperblock;

typedef struct __attribute__((packed)) {
    uint32_t h_magic;
    uint32_t h_blocktype;
    uint32_t h_sequence;
    uint32_t h_checksum;
} JbdBlockHeader;

typedef struct __attribute__((packed)) {
    JbdBlockHeader header;
    uint32_t num_tags;
    uint32_t reserved;
} JbdDescriptorBlock;

typedef struct __attribute__((packed)) {
    JbdBlockHeader header;
    uint32_t h_chksum_type;
    uint32_t h_chksum_size;
    uint32_t h_chksum_offset;
    uint32_t timestamp;
    uint32_t checksum[16];
    uint8_t  h_padding[36];
} JbdCommitBlock;

typedef struct __attribute__((packed)) {
    JbdBlockHeader header;
    uint32_t r_count;
    uint32_t r_reserved;
} JbdRevokeBlock;

typedef struct {
    int        running;
    uint32_t   journal_ino;
    uint32_t   journal_blocks;
    uint32_t   journal_blocksize;
    uint32_t   h_sequence;
    uint32_t   h_start;
    uint32_t   h_tail;
    uint32_t   state;
    uint8_t*   transaction_buffer;
    uint32_t   trans_buf_blocks;
    uint32_t   trans_buf_size;
    uint32_t   trans_buf_used;
    uint32_t   checksum_type;
    volatile int lock;
} JournalState;

typedef struct {
    uint32_t    handle_id;
    uint32_t    sequence;
    uint32_t    commit_block;
    uint32_t    num_buffers;
    uint32_t*   modified_blocks;
    uint32_t    num_modified;
    int         aborted;
} JournalHandle;

int journal_init(void);
int journal_destroy(void);
int journal_format(void);
JournalHandle* journal_start(void);
int journal_extend(JournalHandle* h, uint32_t size);
int journal_stop(JournalHandle* h);
int journal_abort(JournalHandle* h);
int journal_get_write_access(JournalHandle* h, uint32_t block_num);
int journal_dirty_metadata(JournalHandle* h, uint32_t block_num);
uint32_t journal_compute_crc32(const uint8_t* data, uint32_t len);
int journal_verify_checksum(const uint8_t* data, uint32_t len, uint32_t stored_crc);
int journal_recover(void);
int journal_replay_transaction(uint32_t seq);
JournalState* journal_get_state(void);
int journal_add_orphan(JournalHandle* h, uint32_t ino);
int journal_remove_orphan(JournalHandle* h, uint32_t ino);

#endif 