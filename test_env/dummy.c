#include "ufs_internal.h"
#include <string.h>

/* The fake RAM disk */
uint8_t mock_disk[UFS_TOTAL_BLOCKS][UFS_BLOCK_SIZE];

/* The fake global state */
struct ufs_context g_ufs = { 
    .mounted = 1, 
    .sb = { .free_inodes = 511, .free_blocks = 30582 } 
};

/* The fake I/O functions */
int ufs_read_block(uint32_t block_number, void *buffer) {
    if (block_number >= UFS_TOTAL_BLOCKS) return -1;
    memcpy(buffer, mock_disk[block_number], UFS_BLOCK_SIZE);
    return 0;
}

int ufs_write_block(uint32_t block_number, const void *buffer) {
    if (block_number >= UFS_TOTAL_BLOCKS) return -1;
    memcpy(mock_disk[block_number], buffer, UFS_BLOCK_SIZE);
    return 0;
}

int ufs_flush_superblock(void) {
    return 0; 
}

void ufs_init_inode(struct ufs_inode *inode, uint32_t type) {
    memset(inode, 0, sizeof(*inode));
    inode->type = type;
    for (int i = 0; i < UFS_DIRECT_BLOCKS; i++) {
        inode->direct[i] = UFS_INVALID_BLOCK;
    }
    inode->single_indirect = UFS_INVALID_BLOCK;
    inode->double_indirect = UFS_INVALID_BLOCK;
}
