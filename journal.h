#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdint.h>
#include <stddef.h>

/* Maximum number of blocks a single transaction can hold based on 
 * the 512-byte journal header capacity. */
#define UFS_MAX_TX_BLOCKS 124

/* The in-memory staging structure for active transactions */
struct ufs_transaction {
    uint32_t seq;
    uint32_t num_blocks;
    uint32_t target_blocks[UFS_MAX_TX_BLOCKS];
    uint8_t  buffers[UFS_MAX_TX_BLOCKS][512];
};

/* Transaction Lifecycle API */
int ufs_tx_begin(struct ufs_transaction *tx);

int ufs_tx_stage_block(struct ufs_transaction *tx,
                       uint32_t target_block,
                       const void *after_image);

int ufs_tx_commit(struct ufs_transaction *tx);

void ufs_tx_abort(struct ufs_transaction *tx);

/* Mount-time Recovery API */
int ufs_journal_recover(void);

#endif /* JOURNAL_H */