#ifndef UFS_INTERNAL_H
#define UFS_INTERNAL_H

#include "userfs.h"

#include <stdint.h>

/* ---------- General filesystem design ---------- */

#define UFS_MAGIC          0x55465331U
#define UFS_VERSION        1U

#define UFS_TOTAL_BLOCKS   2048U
#define UFS_IMAGE_SIZE     (UFS_TOTAL_BLOCKS * UFS_BLOCK_SIZE)

#define UFS_MAX_INODES     256U
#define UFS_ROOT_INODE     0U

#define UFS_INODE_SIZE     64U
#define UFS_DIRENT_SIZE    64U

#define UFS_DIRECT_BLOCKS  8U
#define UFS_INVALID_BLOCK  UINT32_MAX

/* ---------- Disk-image layout ---------- */

#define UFS_SUPERBLOCK_BLK          0U
#define UFS_INODE_BITMAP_BLK        1U
#define UFS_BLOCK_BITMAP_BLK        2U

#define UFS_INODE_TABLE_START_BLK   3U
#define UFS_INODE_TABLE_BLOCKS      32U

#define UFS_DATA_REGION_START_BLK   35U
#define UFS_DATA_REGION_BLOCKS      2013U

/* ---------- Superblock: exactly 512 bytes ---------- */

struct ufs_superblock {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;

    uint32_t inode_bitmap_start;
    uint32_t inode_bitmap_blocks;

    uint32_t block_bitmap_start;
    uint32_t block_bitmap_blocks;

    uint32_t inode_table_start;
    uint32_t inode_table_blocks;

    uint32_t data_region_start;
    uint32_t data_region_blocks;

    uint32_t total_inodes;
    uint32_t root_inode;

    uint32_t free_inodes;
    uint32_t free_blocks;

    uint8_t padding[448];
};

/* ---------- Inode: exactly 64 bytes ---------- */

struct ufs_inode {
    uint32_t type;
    uint32_t flags;

    uint64_t size;
    uint32_t block_count;

    uint32_t direct[UFS_DIRECT_BLOCKS];

    uint32_t single_indirect;
    uint32_t double_indirect;

    uint32_t reserved;
};

/* ---------- Directory entry: exactly 64 bytes ---------- */

struct ufs_disk_dirent {
    uint32_t used;
    uint32_t inode_number;
    uint32_t type;

    char name[UFS_MAX_NAME + 1];

    uint32_t reserved[5];
};

/* ---------- Temporary in-memory descriptor ---------- */

struct ufs_file_descriptor {
    int in_use;
    uint32_t inode_number;
    off_t offset;
    int flags;
};

/* ---------- Compile-time size checks ---------- */

_Static_assert(sizeof(struct ufs_superblock) == UFS_BLOCK_SIZE,
               "Superblock must be 512 bytes");

_Static_assert(sizeof(struct ufs_inode) == UFS_INODE_SIZE,
               "Inode must be 64 bytes");

_Static_assert(sizeof(struct ufs_disk_dirent) == UFS_DIRENT_SIZE,
               "Directory entry must be 64 bytes");

#endif
