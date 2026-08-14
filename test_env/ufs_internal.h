#ifndef UFS_INTERNAL_H
#define UFS_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ---------- Mocked Version 2 Layout Constants ---------- */

#define UFS_BLOCK_SIZE 512
#define UFS_TOTAL_BLOCKS 32768       /* 16 MiB image */
#define UFS_MAX_INODES 512
#define UFS_INODE_SIZE 128           /* Expanded to 128 bytes */
#define UFS_ROOT_INODE 0

/* Block Pointers */
#define UFS_DIRECT_BLOCKS 8
#define UFS_INVALID_BLOCK UINT32_MAX

/* Region Start Blocks (Based on V2 Specs) */
#define UFS_SUPERBLOCK_BLK 0
#define UFS_INODE_BITMAP_BLK 1

#define UFS_BLOCK_BITMAP_START_BLK 2
#define UFS_BLOCK_BITMAP_BLOCKS 8    /* 8 blocks for the bitmap */

#define UFS_INODE_TABLE_START_BLK 10 /* Blocks 10-137 */
#define UFS_INODE_TABLE_BLOCKS 128

#define UFS_JOURNAL_START_BLK 138    /* Blocks 138-2185 reserved for journal */

#define UFS_DATA_REGION_START_BLK 2186
#define UFS_DATA_REGION_BLOCKS 30582 /* 32768 - 2186 */

/* Journal Magic Numbers */
#define UFS_JOURNAL_MAGIC 0x4A4F5552U
#define UFS_JOURNAL_COMMIT_MAGIC 0x434F4D4DU

/* File Types (Needed for ufs_init_inode) */
#define UFS_TYPE_FILE 1
#define UFS_TYPE_DIR  2
#define UFS_TYPE_LINK 3              /* Symlink added in V2 */

/* ---------- Mocked Structs ---------- */

struct ufs_superblock {
    uint32_t free_inodes;
    uint32_t free_blocks;
    /* (Other superblock fields omitted in the mock to save space, 
     * as Member 2's code only actively mutates the free counts) */
};

struct ufs_context {
    int mounted;
    struct ufs_superblock sb;
};

/* The new 128-byte Inode Struct */
struct ufs_inode {
    uint32_t type;
    uint32_t flags;

    uint64_t size;
    uint32_t block_count;

    uint32_t generation;             /* V2 Generation tracker */
    
    uint32_t uid;                    /* Member 5 permissions (Mocked) */
    uint32_t gid;
    uint32_t mode;
    uint32_t link_count;

    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;

    uint32_t direct[UFS_DIRECT_BLOCKS];
    uint32_t single_indirect;
    uint32_t double_indirect;

    uint32_t reserved[8];            /* Pad out to exactly 128 bytes */
};

/* Provide the global context so userfs_storage.c can check g_ufs.mounted */
extern struct ufs_context g_ufs;

/* ---------- Mocked Lifecycle Functions ---------- */

int ufs_is_mounted(void);
int ufs_read_block(uint32_t block_number, void *buffer);
int ufs_write_block(uint32_t block_number, const void *buffer);
int ufs_flush_superblock(void);
void ufs_init_inode(struct ufs_inode *inode, uint32_t type);

#endif /* UFS_INTERNAL_H */
