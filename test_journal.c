#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "ufs_internal.h"
#include "journal.h"

extern uint8_t mock_disk[UFS_TOTAL_BLOCKS][UFS_BLOCK_SIZE];

/* Dummy checksum function just for the test verification */
static uint32_t calc_checksum(const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i++) sum += bytes[i];
    return sum;
}

void test_transaction_commit(void) {
    struct ufs_transaction tx;
    uint8_t test_data[UFS_BLOCK_SIZE];
    memset(test_data, 0xAA, UFS_BLOCK_SIZE); 
    
    printf("Running: test_transaction_commit...\n");
    
    assert(ufs_tx_begin(&tx) == 0);
    assert(ufs_tx_stage_block(&tx, 5000, test_data) == 0);
    assert(tx.num_blocks == 1);
    
    /* Commit it */
    assert(ufs_tx_commit(&tx) == 0);
    
    /* Verify the data actually made it to physical block 5000 */
    uint8_t read_back[UFS_BLOCK_SIZE];
    ufs_read_block(5000, read_back);
    assert(memcmp(read_back, test_data, UFS_BLOCK_SIZE) == 0);
    
    printf("PASS\n");
}

void test_journal_recovery(void) {
    struct ufs_transaction tx;
    uint8_t test_data[UFS_BLOCK_SIZE];
    memset(test_data, 0xBB, UFS_BLOCK_SIZE);
    
    printf("Running: test_journal_recovery...\n");
    
    ufs_tx_begin(&tx);
    ufs_tx_stage_block(&tx, 6000, test_data);
    
    /* MANUALLY simulate a crash: Write header and data, but skip the final commit copy */
    /* Note: We have to hardcode the header struct here since it's hidden in journal.c */
    struct {
        uint32_t magic; uint32_t seq; uint32_t num_blocks; uint32_t checksum; uint32_t target_blocks[124];
    } header = {0};
    
    header.magic = UFS_JOURNAL_MAGIC;
    header.seq = tx.seq;
    header.num_blocks = 1;
    header.checksum = calc_checksum(test_data, UFS_BLOCK_SIZE);
    header.target_blocks[0] = 6000;
    
    struct { uint32_t magic; uint32_t seq; uint8_t padding[504]; } commit = {0};
    commit.magic = UFS_JOURNAL_COMMIT_MAGIC;
    commit.seq = tx.seq;
    
    ufs_write_block(UFS_JOURNAL_START_BLK, &header);
    ufs_write_block(UFS_JOURNAL_START_BLK + 1, test_data);
    ufs_write_block(UFS_JOURNAL_START_BLK + 2, &commit);
    
    /* Run recovery */
    assert(ufs_journal_recover() == 0);
    
    /* Prove the recovery system applied the staged data to block 6000 */
    uint8_t read_back[UFS_BLOCK_SIZE];
    ufs_read_block(6000, read_back);
    assert(memcmp(read_back, test_data, UFS_BLOCK_SIZE) == 0);
    
    printf("PASS\n");
}

int main(void) {
    memset(mock_disk, 0, UFS_TOTAL_BLOCKS * UFS_BLOCK_SIZE); 
    test_transaction_commit();
    test_journal_recovery();
    printf("All Journal tests passed!\n");
    return 0;
}
