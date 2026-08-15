/*
 * journal.c
 *
 * Member 2 — Storage and full journaling.
 *
 * Implements:
 *   - Transaction begin, staging, commit, and abort.
 *   - 512-byte after-image staging with checksums and sequence numbers.
 *   - Repeatable journal recovery on mount.
 *   - Incomplete transaction ignoring and completed journal clearing.
 */

#include "journal.h"
#include "ufs_internal.h"
#include <errno.h>
#include <string.h>
#include <stdint.h>

/* On-disk Journal Header Block */
struct ufs_journal_header {
    uint32_t magic;
    uint32_t seq;
    uint32_t num_blocks;
    uint32_t checksum;
    uint32_t target_blocks[UFS_MAX_TX_BLOCKS];
};
_Static_assert(sizeof(struct ufs_journal_header) == UFS_BLOCK_SIZE, 
               "Journal header must be exactly 512 bytes");

/* On-disk Journal Commit Block */
struct ufs_journal_commit {
    uint32_t magic;
    uint32_t seq;
    uint8_t  padding[504];
};
_Static_assert(sizeof(struct ufs_journal_commit) == UFS_BLOCK_SIZE, 
               "Journal commit must be exactly 512 bytes");

/* Global sequence tracker for the runtime session */
static uint32_t g_journal_seq = 1;

/* =====================================================================
 * Internal Helpers
 * ===================================================================== */

/* Simple byte-sum checksum for data integrity verification */
static uint32_t calculate_checksum(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i++) {
        sum += bytes[i];
    }
    return sum;
}

/* =====================================================================
 * Transaction Lifecycle
 * ===================================================================== */

int ufs_tx_begin(struct ufs_transaction *tx)
{
    if (tx == NULL) {
        errno = EINVAL;
        return -1;
    }
    
    memset(tx, 0, sizeof(*tx));
    tx->seq = g_journal_seq++;
    tx->num_blocks = 0;
    
    return 0;
}

int ufs_tx_stage_block(struct ufs_transaction *tx, uint32_t target_block, 
                       const void *after_image)
{
    uint32_t i;

    if (tx == NULL || after_image == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (target_block >= UFS_TOTAL_BLOCKS ||
        (target_block >= UFS_JOURNAL_START_BLK &&
         target_block < UFS_JOURNAL_START_BLK + UFS_JOURNAL_BLOCKS)) {
        errno = EINVAL;
        return -1;
    }

    /* A transaction keeps only the newest image for each target block. */
    for (i = 0; i < tx->num_blocks; ++i) {
        if (tx->target_blocks[i] == target_block) {
            memcpy(tx->buffers[i], after_image, UFS_BLOCK_SIZE);
            return 0;
        }
    }
    
    /* Enforce journal capacity limit. Operations exceeding this must be 
     * split by the caller. */
    if (tx->num_blocks >= UFS_MAX_TX_BLOCKS) {
        errno = ENOSPC;
        return -1;
    }
    
    tx->target_blocks[tx->num_blocks] = target_block;
    memcpy(tx->buffers[tx->num_blocks], after_image, UFS_BLOCK_SIZE);
    tx->num_blocks++;
    
    return 0;
}

int ufs_tx_stage_inode(struct ufs_transaction *tx,
                       uint32_t inode_number,
                       const struct ufs_inode *inode)
{
    uint32_t inodes_per_block;
    uint32_t block_number;
    uint32_t offset;
    uint32_t i;
    uint8_t block[UFS_BLOCK_SIZE];

    if (tx == NULL || inode == NULL || inode_number >= UFS_MAX_INODES) {
        errno = EINVAL;
        return -1;
    }

    inodes_per_block = UFS_BLOCK_SIZE / UFS_INODE_SIZE;
    block_number = UFS_INODE_TABLE_START_BLK +
                   inode_number / inodes_per_block;
    offset = (inode_number % inodes_per_block) * UFS_INODE_SIZE;

    for (i = 0; i < tx->num_blocks; ++i) {
        if (tx->target_blocks[i] == block_number) {
            memcpy(block, tx->buffers[i], sizeof(block));
            memcpy(block + offset, inode, sizeof(*inode));
            return ufs_tx_stage_block(tx, block_number, block);
        }
    }

    if (ufs_read_block(block_number, block) < 0) {
        return -1;
    }
    memcpy(block + offset, inode, sizeof(*inode));
    return ufs_tx_stage_block(tx, block_number, block);
}

void ufs_tx_abort(struct ufs_transaction *tx)
{
    if (tx != NULL) {
        /* Scrub the staged buffers and reset state to prevent leaks */
        memset(tx, 0, sizeof(*tx));
    }
}

int ufs_tx_commit(struct ufs_transaction *tx)
{
    if (tx == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (tx->num_blocks == 0) {
        return 0; /* Nothing to commit */
    }

    struct ufs_journal_header header;
    memset(&header, 0, sizeof(header));
    header.magic = UFS_JOURNAL_MAGIC;
    header.seq = tx->seq;
    header.num_blocks = tx->num_blocks;
    
    uint32_t data_checksum = 0;
    for (uint32_t i = 0; i < tx->num_blocks; i++) {
        header.target_blocks[i] = tx->target_blocks[i];
        data_checksum += calculate_checksum(tx->buffers[i], UFS_BLOCK_SIZE);
    }
    header.checksum = data_checksum;

    /* 1. Write the Header Block */
    if (ufs_write_block(UFS_JOURNAL_START_BLK, &header) != 0) {
        return -1;
    }

    /* 2. Write the staged After-Images to the journal region */
    for (uint32_t i = 0; i < tx->num_blocks; i++) {
        if (ufs_write_block(UFS_JOURNAL_START_BLK + 1 + i, tx->buffers[i]) != 0) {
            return -1;
        }
    }

    if (ufs_sync_image() < 0) {
        return -1;
    }

    /* 3. Write the Commit Record (Transaction is now safely on disk) */
    struct ufs_journal_commit commit;
    memset(&commit, 0, sizeof(commit));
    commit.magic = UFS_JOURNAL_COMMIT_MAGIC;
    commit.seq = tx->seq;
    
    if (ufs_write_block(UFS_JOURNAL_COMMIT_BLK, &commit) != 0 ||
        ufs_sync_image() < 0) {
        return -1;
    }

    /* --- DATA IS PERSISTENT. BEGIN APPLYING TO TARGET BLOCKS --- */

    /* 4. Replay blocks to their final physical locations */
    for (uint32_t i = 0; i < tx->num_blocks; i++) {
        /* If a crash happens during this loop, ufs_journal_recover() will 
         * safely complete the copying process on the next mount. */
        if (ufs_write_block(tx->target_blocks[i], tx->buffers[i]) != 0) {
            return -1;
        }
    }

    if (ufs_sync_image() < 0) {
        return -1;
    }

    /* 5. Clear completed journal to prevent double-replaying later */
    memset(&header, 0, sizeof(header)); 
    memset(&commit, 0, sizeof(commit));
    if (ufs_write_block(UFS_JOURNAL_START_BLK, &header) != 0 ||
        ufs_write_block(UFS_JOURNAL_COMMIT_BLK, &commit) != 0 ||
        ufs_sync_image() < 0) {
        return -1;
    }

    return 0;
}

/* =====================================================================
 * Mount-time Recovery
 * ===================================================================== */

int ufs_journal_recover(void)
{
    struct ufs_journal_header header;
    
    if (ufs_read_block(UFS_JOURNAL_START_BLK, &header) != 0) {
        return -1;
    }

    /* If there is no magic number, the journal is empty/clean. */
    if (header.magic != UFS_JOURNAL_MAGIC) {
        return 0;
    }

    /* Ensure block count is sane to prevent out-of-bounds reads */
    if (header.num_blocks == 0 || header.num_blocks > UFS_MAX_TX_BLOCKS) {
        goto clear_journal;
    }

    /* Check for the commit block. If missing, transaction is incomplete. */
    struct ufs_journal_commit commit;
    if (ufs_read_block(UFS_JOURNAL_COMMIT_BLK, &commit) != 0) {
        return -1;
    }

    if (commit.magic != UFS_JOURNAL_COMMIT_MAGIC || commit.seq != header.seq) {
        /* Transaction aborted mid-write due to power loss. Ignore it. */
        goto clear_journal;
    }

    /* Verify data integrity via checksum */
    uint32_t verified_checksum = 0;
    uint8_t buffer[UFS_BLOCK_SIZE];
    
    for (uint32_t i = 0; i < header.num_blocks; i++) {
        if (header.target_blocks[i] >= UFS_TOTAL_BLOCKS ||
            (header.target_blocks[i] >= UFS_JOURNAL_START_BLK &&
             header.target_blocks[i] <
                 UFS_JOURNAL_START_BLK + UFS_JOURNAL_BLOCKS)) {
            goto clear_journal;
        }
        if (ufs_read_block(UFS_JOURNAL_START_BLK + 1 + i, buffer) != 0) {
            return -1;
        }
        verified_checksum += calculate_checksum(buffer, UFS_BLOCK_SIZE);
    }

    if (verified_checksum != header.checksum) {
        /* Corrupted payload data. Ignore it to prevent disk corruption. */
        goto clear_journal;
    }

    /* --- REPLAY COMMITTED TRANSACTION --- */
    for (uint32_t i = 0; i < header.num_blocks; i++) {
        if (ufs_read_block(UFS_JOURNAL_START_BLK + 1 + i, buffer) != 0) {
            return -1;
        }
        if (ufs_write_block(header.target_blocks[i], buffer) != 0) {
            return -1;
        }
    }

    if (ufs_sync_image() < 0) {
        return -1;
    }

clear_journal:
    /* Wipe the magic number to mark the journal as completely applied */
    memset(&header, 0, sizeof(header));
    memset(&commit, 0, sizeof(commit));
    if (ufs_write_block(UFS_JOURNAL_START_BLK, &header) < 0 ||
        ufs_write_block(UFS_JOURNAL_COMMIT_BLK, &commit) < 0) {
        return -1;
    }
    return ufs_sync_image();
}
