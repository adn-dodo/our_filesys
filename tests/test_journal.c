#include "journal.h"
#include "ufs_internal.h"
#include "userfs.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_IMAGE "journal_test.img"

struct test_journal_header {
    uint32_t magic;
    uint32_t seq;
    uint32_t num_blocks;
    uint32_t checksum;
    uint32_t target_blocks[UFS_MAX_TX_BLOCKS];
};

struct test_journal_commit {
    uint32_t magic;
    uint32_t seq;
    uint8_t padding[504];
};

static uint32_t checksum(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i < size; ++i) {
        sum += bytes[i];
    }
    return sum;
}

static void test_commit(void)
{
    struct ufs_transaction tx;
    uint8_t expected[UFS_BLOCK_SIZE];
    uint8_t actual[UFS_BLOCK_SIZE];

    memset(expected, 0xA5, sizeof(expected));
    assert(ufs_tx_begin(&tx) == 0);
    assert(ufs_tx_stage_block(&tx, UFS_DATA_REGION_START_BLK,
                              expected) == 0);
    assert(ufs_tx_commit(&tx) == 0);
    assert(ufs_read_block(UFS_DATA_REGION_START_BLK, actual) == 0);
    assert(memcmp(expected, actual, sizeof(expected)) == 0);
    puts("[PASS] committed after-image reaches its home block");
}

static void test_recovery(void)
{
    struct test_journal_header header;
    struct test_journal_commit commit;
    uint8_t expected[UFS_BLOCK_SIZE];
    uint8_t actual[UFS_BLOCK_SIZE];
    const uint32_t target = UFS_DATA_REGION_START_BLK + 1U;

    memset(&header, 0, sizeof(header));
    memset(&commit, 0, sizeof(commit));
    memset(expected, 0x3C, sizeof(expected));
    header.magic = UFS_JOURNAL_MAGIC;
    header.seq = 77U;
    header.num_blocks = 1U;
    header.checksum = checksum(expected, sizeof(expected));
    header.target_blocks[0] = target;
    commit.magic = UFS_JOURNAL_COMMIT_MAGIC;
    commit.seq = header.seq;

    assert(ufs_write_block(UFS_JOURNAL_START_BLK, &header) == 0);
    assert(ufs_write_block(UFS_JOURNAL_START_BLK + 1U, expected) == 0);
    assert(ufs_write_block(UFS_JOURNAL_COMMIT_BLK, &commit) == 0);
    assert(ufs_sync_image() == 0);
    assert(ufs_journal_recover() == 0);
    assert(ufs_read_block(target, actual) == 0);
    assert(memcmp(expected, actual, sizeof(expected)) == 0);
    puts("[PASS] recovery replays a valid committed transaction");
}

static void test_incomplete_transaction(void)
{
    struct test_journal_header header;
    uint8_t original[UFS_BLOCK_SIZE];
    uint8_t staged[UFS_BLOCK_SIZE];
    uint8_t actual[UFS_BLOCK_SIZE];
    const uint32_t target = UFS_DATA_REGION_START_BLK + 2U;

    memset(original, 0x11, sizeof(original));
    memset(staged, 0x22, sizeof(staged));
    memset(&header, 0, sizeof(header));
    assert(ufs_write_block(target, original) == 0);

    header.magic = UFS_JOURNAL_MAGIC;
    header.seq = 88U;
    header.num_blocks = 1U;
    header.checksum = checksum(staged, sizeof(staged));
    header.target_blocks[0] = target;
    assert(ufs_write_block(UFS_JOURNAL_START_BLK, &header) == 0);
    assert(ufs_write_block(UFS_JOURNAL_START_BLK + 1U, staged) == 0);

    assert(ufs_journal_recover() == 0);
    assert(ufs_read_block(target, actual) == 0);
    assert(memcmp(original, actual, sizeof(original)) == 0);
    puts("[PASS] recovery ignores an incomplete transaction");
}

int main(void)
{
    assert(ufs_format(TEST_IMAGE, UFS_IMAGE_SIZE) == 0);
    assert(ufs_mount(TEST_IMAGE) == 0);
    test_commit();
    test_recovery();
    test_incomplete_transaction();
    assert(ufs_unmount() == 0);
    puts("Journal tests passed: 3");
    return 0;
}
