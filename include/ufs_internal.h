#ifndef UFS_INTERNAL_H
#define UFS_INTERNAL_H

#include "userfs.h"

#include <stdint.h>

/* ---------- General filesystem design ---------- */

#define UFS_MAGIC          0x55465331U
#define UFS_VERSION        2U

#define UFS_TOTAL_BLOCKS   32768U
#define UFS_IMAGE_SIZE     (UFS_TOTAL_BLOCKS * UFS_BLOCK_SIZE)

#define UFS_MAX_INODES     512U
#define UFS_ROOT_INODE     0U

#define UFS_INODE_SIZE     128U
#define UFS_DIRENT_SIZE    64U

#define UFS_DIRECT_BLOCKS  8U
#define UFS_INVALID_BLOCK  UINT32_MAX

/* ---------- Disk-image layout ---------- */

#define UFS_SUPERBLOCK_BLK          0U
#define UFS_INODE_BITMAP_BLK        1U
#define UFS_INODE_BITMAP_BLOCKS     1U
#define UFS_BLOCK_BITMAP_BLK        2U
#define UFS_BLOCK_BITMAP_START_BLK  UFS_BLOCK_BITMAP_BLK
#define UFS_BLOCK_BITMAP_BLOCKS     8U

#define UFS_INODE_TABLE_START_BLK   10U
#define UFS_INODE_TABLE_BLOCKS      128U

#define UFS_JOURNAL_START_BLK       138U
#define UFS_JOURNAL_BLOCKS          2048U
#define UFS_JOURNAL_COMMIT_BLK      (UFS_JOURNAL_START_BLK + UFS_JOURNAL_BLOCKS - 1U)

#define UFS_DATA_REGION_START_BLK   2186U
#define UFS_DATA_REGION_BLOCKS      30582U

#define UFS_JOURNAL_MAGIC           0x4A4F5552U
#define UFS_JOURNAL_COMMIT_MAGIC    0x434F4D4DU
#define UFS_STATE_CLEAN             0U
#define UFS_STATE_DIRTY             1U
#define UFS_FEATURE_JOURNAL         0x00000001U
#define UFS_FEATURE_METADATA        0x00000002U
#define UFS_FEATURE_LINKS           0x00000004U

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

    uint32_t journal_start;
    uint32_t journal_blocks;
    uint32_t journal_sequence;
    uint32_t state;
    uint32_t features;

    uint8_t padding[428];
};

/* ---------- Persistent timestamp: exactly 16 bytes ---------- */

struct ufs_disk_time {
    int64_t sec;
    int32_t nsec;
    uint32_t reserved;
};

/* ---------- Inode: exactly 128 bytes ---------- */

struct ufs_inode {
    uint32_t type;
    uint32_t flags;

    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t link_count;

    uint64_t size;
    uint32_t block_count;

    uint32_t direct[UFS_DIRECT_BLOCKS];

    uint32_t single_indirect;
    uint32_t double_indirect;

    uint32_t generation;

    struct ufs_disk_time atime;
    struct ufs_disk_time mtime;
    struct ufs_disk_time ctime;
};

/* ---------- Directory entry: exactly 64 bytes ---------- */

struct ufs_disk_dirent {
    uint32_t used;
    uint32_t inode_number;
    uint32_t type;

    char name[UFS_MAX_NAME + 1];

    uint32_t reserved[5];
};

/* ---------- Shared mounted-image state ---------- */

struct ufs_context {
    int mounted;
    int fd;
    struct ufs_superblock sb;
};

extern struct ufs_context g_ufs;

/* Lifecycle-owned services used by the other implementation modules. */
int ufs_is_mounted(void);
int ufs_read_block(uint32_t block_number, void *buffer);
int ufs_write_block(uint32_t block_number, const void *buffer);
int ufs_flush_superblock(void);
int ufs_sync_image(void);

/* Put a new inode into a safe, empty state with invalid block pointers. */
void ufs_init_inode(struct ufs_inode *inode, uint32_t type);

/* ---------- Compile-time size checks ---------- */

_Static_assert(sizeof(struct ufs_superblock) == UFS_BLOCK_SIZE,
               "Superblock must be 512 bytes");

_Static_assert(sizeof(struct ufs_inode) == UFS_INODE_SIZE,
               "Inode must be 128 bytes");

_Static_assert(sizeof(struct ufs_disk_time) == 16U,
               "Disk timestamp must be 16 bytes");

_Static_assert(sizeof(struct ufs_disk_dirent) == UFS_DIRENT_SIZE,
               "Directory entry must be 64 bytes");

#endif
