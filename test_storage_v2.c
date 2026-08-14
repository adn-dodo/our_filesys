#include <stdio.h>
#include <string.h>
#include <assert.h>

/* The -I flag during compilation will route this to test_env/ufs_internal.h */
#include "ufs_internal.h"
#include "userfs_storage.h"

/* Access the fake disk array from dummy.c to verify data manually if needed */
extern uint8_t mock_disk[UFS_TOTAL_BLOCKS][UFS_BLOCK_SIZE];

void test_inode_generation(void) {
    uint32_t inode1, inode2;
    struct ufs_inode check_inode;

    printf("Running: test_inode_generation...\n");
    
    assert(allocate_inode(&inode1) == 0);
    assert(inode1 == 1); /* Skips root (0) */
    
    read_inode(inode1, &check_inode);
    assert(check_inode.generation == 1);
    
    assert(free_inode(inode1) == 0);
    
    assert(allocate_inode(&inode2) == 0);
    assert(inode2 == inode1); /* Should reuse the same slot */
    
    read_inode(inode2, &check_inode);
    assert(check_inode.generation == 2); /* Generation must increment! */
    
    printf("PASS\n");
}

void test_data_block_boundaries(void) {
    uint32_t block_num;
    
    printf("Running: test_data_block_boundaries...\n");
    
    assert(allocate_data_block(&block_num) == 0);
    /* Must not allocate metadata or journal blocks (0 - 2185) */
    assert(block_num >= UFS_DATA_REGION_START_BLK);
    
    assert(free_data_block(block_num) == 0);
    
    printf("PASS\n");
}

int main(void) {
    memset(mock_disk, 0, UFS_TOTAL_BLOCKS * UFS_BLOCK_SIZE); /* Format mock disk */
    test_inode_generation();
    test_data_block_boundaries();
    printf("All Storage V2 tests passed!\n");
    return 0;
}
