#include "ufs_internal.h"
#include "userfs.h"
#include "userfs_storage.h"

#include <assert.h>
#include <stdio.h>

#define TEST_IMAGE "storage_test.img"

static void test_inode_generation(void)
{
    struct ufs_inode inode;
    uint32_t first;
    uint32_t second;

    assert(allocate_inode(&first) == 0);
    assert(first == 1U);
    assert(read_inode(first, &inode) == 0);
    assert(inode.generation == 1U);
    assert(free_inode(first) == 0);
    assert(allocate_inode(&second) == 0);
    assert(second == first);
    assert(read_inode(second, &inode) == 0);
    assert(inode.generation == 2U);
    assert(free_inode(second) == 0);
    puts("[PASS] inode generation increments after slot reuse");
}

static void test_data_block_reuse(void)
{
    uint32_t first;
    uint32_t second;

    assert(allocate_data_block(&first) == 0);
    assert(first == UFS_DATA_REGION_START_BLK);
    assert(free_data_block(first) == 0);
    assert(allocate_data_block(&second) == 0);
    assert(second == first);
    assert(free_data_block(second) == 0);
    puts("[PASS] data allocation starts at block 2186 and reuses frees");
}

int main(void)
{
    assert(ufs_format(TEST_IMAGE, UFS_IMAGE_SIZE) == 0);
    assert(ufs_mount(TEST_IMAGE) == 0);
    test_inode_generation();
    test_data_block_reuse();
    assert(ufs_unmount() == 0);
    puts("Storage V2 tests passed: 2");
    return 0;
}
