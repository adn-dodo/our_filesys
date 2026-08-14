/*
 * userfs_storage.c
 *
 * Member 2 — Bitmaps, inodes and block allocation.
 *
 * Implements:
 *   - Bitmap primitives (test/set/clear) and inode/block bitmap I/O
 *   - Inode table read/write/allocate/free
 *   - Data block allocate/free/zero
 *   - Logical-to-physical block translation across direct, single-indirect
 *     and double-indirect pointers, including lazy allocation
 *   - ufs_truncate() (public) and ufs_truncate_inode() (internal worker)
 *
 * The shared lifecycle services and on-disk structures are declared in
 * ufs_internal.h. Public path-based truncate is implemented here using
 * resolve_path() from the namespace module.
 */
#include "userfs_storage.h"
#include "namespace.h"
#include <errno.h>
#include <string.h>

/* ---------- Local constants derived from the on-disk layout ---------- */

#define UFS_PTRS_PER_BLOCK \
    (UFS_BLOCK_SIZE / (uint32_t)sizeof(uint32_t))              /* 128 */

#define UFS_SINGLE_INDIRECT_CAPACITY UFS_PTRS_PER_BLOCK          /* 128 */

#define UFS_DOUBLE_INDIRECT_CAPACITY \
    (UFS_PTRS_PER_BLOCK * UFS_PTRS_PER_BLOCK)                    /* 16384 */

#define UFS_MAX_FILE_BLOCKS \
    (UFS_DIRECT_BLOCKS + UFS_SINGLE_INDIRECT_CAPACITY + UFS_DOUBLE_INDIRECT_CAPACITY)

/* =====================================================================
 * Bit operations
 * ===================================================================== */

int bitmap_test(const uint8_t *bitmap, uint32_t bit)
{
    return (bitmap[bit / 8] >> (bit % 8)) & 1U;
}

void bitmap_set(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8] = (uint8_t)(bitmap[bit / 8] | (uint8_t)(1U << (bit % 8)));
}

void bitmap_clear(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8] = (uint8_t)(bitmap[bit / 8] & (uint8_t)~(1U << (bit % 8)));
}

/* =====================================================================
 * Bitmap block I/O
 * ===================================================================== */

int read_inode_bitmap(uint8_t *bitmap)
{
    if (!g_ufs.mounted) {
        errno = EINVAL;
        return -1;
    }
    return ufs_read_block(UFS_INODE_BITMAP_BLK, bitmap);
}

int write_inode_bitmap(const uint8_t *bitmap)
{
    if (!g_ufs.mounted) {
        errno = EINVAL;
        return -1;
    }
    return ufs_write_block(UFS_INODE_BITMAP_BLK, bitmap);
}

int read_block_bitmap(uint8_t *bitmap)
{
    if (!g_ufs.mounted) {
        errno = EINVAL;
        return -1;
    }
    return ufs_read_block(UFS_BLOCK_BITMAP_BLK, bitmap);
}

int write_block_bitmap(const uint8_t *bitmap)
{
    if (!g_ufs.mounted) {
        errno = EINVAL;
        return -1;
    }
    return ufs_write_block(UFS_BLOCK_BITMAP_BLK, bitmap);
}

/* =====================================================================
 * Inode table read / write
 * ===================================================================== */

int read_inode(uint32_t inode_num, struct ufs_inode *inode)
{
    if (!g_ufs.mounted || inode == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (inode_num >= UFS_MAX_INODES) {
        errno = EINVAL;
        return -1;
    }

    uint32_t inodes_per_block = UFS_BLOCK_SIZE / UFS_INODE_SIZE;
    uint32_t block = UFS_INODE_TABLE_START_BLK + inode_num / inodes_per_block;
    uint32_t offset_in_block = (inode_num % inodes_per_block) * UFS_INODE_SIZE;

    uint8_t buf[UFS_BLOCK_SIZE];
    if (ufs_read_block(block, buf) != 0) {
        return -1;
    }

    memcpy(inode, buf + offset_in_block, sizeof(*inode));
    return 0;
}

int write_inode(uint32_t inode_num, const struct ufs_inode *inode)
{
    if (!g_ufs.mounted || inode == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (inode_num >= UFS_MAX_INODES) {
        errno = EINVAL;
        return -1;
    }

    uint32_t inodes_per_block = UFS_BLOCK_SIZE / UFS_INODE_SIZE;
    uint32_t block = UFS_INODE_TABLE_START_BLK + inode_num / inodes_per_block;
    uint32_t offset_in_block = (inode_num % inodes_per_block) * UFS_INODE_SIZE;

    uint8_t buf[UFS_BLOCK_SIZE];
    if (ufs_read_block(block, buf) != 0) {
        return -1;
    }

    memcpy(buf + offset_in_block, inode, sizeof(*inode));
    return ufs_write_block(block, buf);
}

/* =====================================================================
 * Inode allocation / release
 * ===================================================================== */

int allocate_inode(uint32_t *inode_num_out)
{
    if (!g_ufs.mounted || inode_num_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    uint8_t bitmap[UFS_BLOCK_SIZE];
    if (read_inode_bitmap(bitmap) != 0) {
        return -1;
    }

    for (uint32_t i = 1; i < UFS_MAX_INODES; i++) {
        if (bitmap_test(bitmap, i)) {
            continue;
        }

        bitmap_set(bitmap, i);
        if (write_inode_bitmap(bitmap) != 0) {
            return -1;
        }

        struct ufs_inode inode;
        ufs_init_inode(&inode, 0);

        if (write_inode(i, &inode) != 0) {
            /* Roll back the bitmap bit so we don't leak the inode slot. */
            bitmap_clear(bitmap, i);
            write_inode_bitmap(bitmap);
            return -1;
        }

        g_ufs.sb.free_inodes--;
        (void)ufs_flush_superblock();

        *inode_num_out = i;
        return 0;
    }

    errno = ENOSPC;
    return -1;
}

int free_inode(uint32_t inode_num)
{
    if (!g_ufs.mounted) {
        errno = EINVAL;
        return -1;
    }
    if (inode_num >= UFS_MAX_INODES) {
        errno = EINVAL;
        return -1;
    }
    if (inode_num == UFS_ROOT_INODE) {
        errno = EBUSY;
        return -1;
    }

    uint8_t bitmap[UFS_BLOCK_SIZE];
    if (read_inode_bitmap(bitmap) != 0) {
        return -1;
    }

    if (!bitmap_test(bitmap, inode_num)) {
        /* Already free; nothing to do. */
        return 0;
    }

    struct ufs_inode inode;
    if (read_inode(inode_num, &inode) != 0) {
        return -1;
    }

    /* Release every data and pointer block owned by this inode first. */
    if (ufs_truncate_inode(inode_num, &inode, 0) != 0) {
        return -1;
    }

    ufs_init_inode(&inode, 0);
    if (write_inode(inode_num, &inode) != 0) {
        return -1;
    }

    bitmap_clear(bitmap, inode_num);
    if (write_inode_bitmap(bitmap) != 0) {
        return -1;
    }

    g_ufs.sb.free_inodes++;
    (void)ufs_flush_superblock();

    return 0;
}

/* =====================================================================
 * Data block allocation / release
 * ===================================================================== */

int zero_block(uint32_t block_num)
{
    uint8_t zero[UFS_BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));
    return ufs_write_block(block_num, zero);
}

/* Fills a block with UFS_INVALID_BLOCK sentinels; used for freshly
 * allocated single/double indirect pointer blocks so every slot reads
 * as "unallocated" rather than the zero-fill that zero_block() would
 * otherwise leave behind. */
static int init_pointer_block(uint32_t block_num)
{
    uint32_t ptrs[UFS_PTRS_PER_BLOCK];
    for (uint32_t i = 0; i < UFS_PTRS_PER_BLOCK; i++) {
        ptrs[i] = UFS_INVALID_BLOCK;
    }
    return ufs_write_block(block_num, ptrs);
}

int allocate_data_block(uint32_t *block_num_out)
{
    if (!g_ufs.mounted || block_num_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    uint8_t bitmap[UFS_BLOCK_SIZE];
    if (read_block_bitmap(bitmap) != 0) {
        return -1;
    }

    /* Metadata blocks 0-34 are never scanned, so they can never be
     * (re)allocated even if a caller passes a bad bitmap in by mistake. */
    for (uint32_t b = UFS_DATA_REGION_START_BLK; b < UFS_TOTAL_BLOCKS; b++) {
        if (bitmap_test(bitmap, b)) {
            continue;
        }

        bitmap_set(bitmap, b);
        if (write_block_bitmap(bitmap) != 0) {
            return -1;
        }

        if (zero_block(b) != 0) {
            bitmap_clear(bitmap, b);
            write_block_bitmap(bitmap);
            return -1;
        }

        g_ufs.sb.free_blocks--;
        (void)ufs_flush_superblock();

        *block_num_out = b;
        return 0;
    }

    errno = ENOSPC;
    return -1;
}

int free_data_block(uint32_t block_num)
{
    if (!g_ufs.mounted) {
        errno = EINVAL;
        return -1;
    }
    if (block_num < UFS_DATA_REGION_START_BLK || block_num >= UFS_TOTAL_BLOCKS) {
        /* Never allow freeing metadata blocks (0-34) or out-of-range blocks. */
        errno = EINVAL;
        return -1;
    }

    uint8_t bitmap[UFS_BLOCK_SIZE];
    if (read_block_bitmap(bitmap) != 0) {
        return -1;
    }

    if (!bitmap_test(bitmap, block_num)) {
        /* Already free; nothing to do. */
        return 0;
    }

    bitmap_clear(bitmap, block_num);
    if (write_block_bitmap(bitmap) != 0) {
        return -1;
    }

    g_ufs.sb.free_blocks++;
    (void)ufs_flush_superblock();

    return 0;
}

/* =====================================================================
 * Logical block -> physical block translation
 * ===================================================================== */

int get_inode_data_block(const struct ufs_inode *inode, uint32_t logical_block,
                          uint32_t *phys_block_out)
{
    if (inode == NULL || phys_block_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (logical_block < UFS_DIRECT_BLOCKS) {
        *phys_block_out = inode->direct[logical_block];
        return 0;
    }
    logical_block -= UFS_DIRECT_BLOCKS;

    if (logical_block < UFS_SINGLE_INDIRECT_CAPACITY) {
        if (inode->single_indirect == UFS_INVALID_BLOCK) {
            *phys_block_out = UFS_INVALID_BLOCK;
            return 0;
        }
        uint32_t ptrs[UFS_PTRS_PER_BLOCK];
        if (ufs_read_block(inode->single_indirect, ptrs) != 0) {
            return -1;
        }
        *phys_block_out = ptrs[logical_block];
        return 0;
    }
    logical_block -= UFS_SINGLE_INDIRECT_CAPACITY;

    if (logical_block < UFS_DOUBLE_INDIRECT_CAPACITY) {
        if (inode->double_indirect == UFS_INVALID_BLOCK) {
            *phys_block_out = UFS_INVALID_BLOCK;
            return 0;
        }
        uint32_t l1[UFS_PTRS_PER_BLOCK];
        if (ufs_read_block(inode->double_indirect, l1) != 0) {
            return -1;
        }
        uint32_t idx1 = logical_block / UFS_PTRS_PER_BLOCK;
        uint32_t idx2 = logical_block % UFS_PTRS_PER_BLOCK;
        if (l1[idx1] == UFS_INVALID_BLOCK) {
            *phys_block_out = UFS_INVALID_BLOCK;
            return 0;
        }
        uint32_t l2[UFS_PTRS_PER_BLOCK];
        if (ufs_read_block(l1[idx1], l2) != 0) {
            return -1;
        }
        *phys_block_out = l2[idx2];
        return 0;
    }

    errno = EFBIG;
    return -1;
}

int ensure_inode_data_block(uint32_t inode_num, struct ufs_inode *inode,
                             uint32_t logical_block, uint32_t *phys_block_out)
{
    if (inode == NULL || phys_block_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (logical_block >= UFS_MAX_FILE_BLOCKS) {
        errno = EFBIG;
        return -1;
    }

    int inode_dirty = 0;
    uint32_t phys = UFS_INVALID_BLOCK;

    if (logical_block < UFS_DIRECT_BLOCKS) {
        if (inode->direct[logical_block] == UFS_INVALID_BLOCK) {
            uint32_t nb;
            if (allocate_data_block(&nb) != 0) {
                return -1;
            }
            inode->direct[logical_block] = nb;
            inode_dirty = 1;
        }
        phys = inode->direct[logical_block];

    } else if (logical_block - UFS_DIRECT_BLOCKS < UFS_SINGLE_INDIRECT_CAPACITY) {
        uint32_t lb = logical_block - UFS_DIRECT_BLOCKS;

        if (inode->single_indirect == UFS_INVALID_BLOCK) {
            uint32_t nb;
            if (allocate_data_block(&nb) != 0) {
                return -1;
            }
            if (init_pointer_block(nb) != 0) {
                free_data_block(nb);
                return -1;
            }
            inode->single_indirect = nb;
            inode_dirty = 1;
        }

        uint32_t ptrs[UFS_PTRS_PER_BLOCK];
        if (ufs_read_block(inode->single_indirect, ptrs) != 0) {
            return -1;
        }
        if (ptrs[lb] == UFS_INVALID_BLOCK) {
            uint32_t nb;
            if (allocate_data_block(&nb) != 0) {
                return -1;
            }
            ptrs[lb] = nb;
            if (ufs_write_block(inode->single_indirect, ptrs) != 0) {
                free_data_block(nb);
                return -1;
            }
        }
        phys = ptrs[lb];

    } else {
        uint32_t lb2 = logical_block - UFS_DIRECT_BLOCKS - UFS_SINGLE_INDIRECT_CAPACITY;

        if (inode->double_indirect == UFS_INVALID_BLOCK) {
            uint32_t nb;
            if (allocate_data_block(&nb) != 0) {
                return -1;
            }
            if (init_pointer_block(nb) != 0) {
                free_data_block(nb);
                return -1;
            }
            inode->double_indirect = nb;
            inode_dirty = 1;
        }

        uint32_t l1[UFS_PTRS_PER_BLOCK];
        if (ufs_read_block(inode->double_indirect, l1) != 0) {
            return -1;
        }

        uint32_t idx1 = lb2 / UFS_PTRS_PER_BLOCK;
        uint32_t idx2 = lb2 % UFS_PTRS_PER_BLOCK;

        if (l1[idx1] == UFS_INVALID_BLOCK) {
            uint32_t nb;
            if (allocate_data_block(&nb) != 0) {
                return -1;
            }
            if (init_pointer_block(nb) != 0) {
                free_data_block(nb);
                return -1;
            }
            l1[idx1] = nb;
            if (ufs_write_block(inode->double_indirect, l1) != 0) {
                free_data_block(nb);
                return -1;
            }
        }

        uint32_t l2[UFS_PTRS_PER_BLOCK];
        if (ufs_read_block(l1[idx1], l2) != 0) {
            return -1;
        }
        if (l2[idx2] == UFS_INVALID_BLOCK) {
            uint32_t nb;
            if (allocate_data_block(&nb) != 0) {
                return -1;
            }
            l2[idx2] = nb;
            if (ufs_write_block(l1[idx1], l2) != 0) {
                free_data_block(nb);
                return -1;
            }
        }
        phys = l2[idx2];
    }

    if (inode_dirty) {
        if (write_inode(inode_num, inode) != 0) {
            return -1;
        }
    }

    *phys_block_out = phys;
    return 0;
}

int free_inode_blocks_after(uint32_t inode_num, struct ufs_inode *inode,
                             uint32_t keep_blocks)
{
    if (inode == NULL) {
        errno = EINVAL;
        return -1;
    }

    int inode_dirty = 0;

    /* ---- Direct blocks ---- */
    uint32_t direct_start = (keep_blocks < UFS_DIRECT_BLOCKS) ? keep_blocks : UFS_DIRECT_BLOCKS;
    for (uint32_t i = direct_start; i < UFS_DIRECT_BLOCKS; i++) {
        if (inode->direct[i] != UFS_INVALID_BLOCK) {
            if (free_data_block(inode->direct[i]) != 0) {
                return -1;
            }
            inode->direct[i] = UFS_INVALID_BLOCK;
            inode_dirty = 1;
        }
    }

    /* ---- Single indirect ---- */
    if (inode->single_indirect != UFS_INVALID_BLOCK) {
        uint32_t ptrs[UFS_PTRS_PER_BLOCK];
        if (ufs_read_block(inode->single_indirect, ptrs) != 0) {
            return -1;
        }

        uint32_t start = (keep_blocks > UFS_DIRECT_BLOCKS)
                              ? (keep_blocks - UFS_DIRECT_BLOCKS)
                              : 0;
        int changed = 0;
        int any_remaining = 0;

        for (uint32_t i = 0; i < UFS_PTRS_PER_BLOCK; i++) {
            if (i >= start) {
                if (ptrs[i] != UFS_INVALID_BLOCK) {
                    if (free_data_block(ptrs[i]) != 0) {
                        return -1;
                    }
                    ptrs[i] = UFS_INVALID_BLOCK;
                    changed = 1;
                }
            } else if (ptrs[i] != UFS_INVALID_BLOCK) {
                any_remaining = 1;
            }
        }

        if (changed) {
            if (ufs_write_block(inode->single_indirect, ptrs) != 0) {
                return -1;
            }
        }

        if (!any_remaining && start == 0) {
            if (free_data_block(inode->single_indirect) != 0) {
                return -1;
            }
            inode->single_indirect = UFS_INVALID_BLOCK;
            inode_dirty = 1;
        }
    }

    /* ---- Double indirect ---- */
    if (inode->double_indirect != UFS_INVALID_BLOCK) {
        uint32_t l1[UFS_PTRS_PER_BLOCK];
        if (ufs_read_block(inode->double_indirect, l1) != 0) {
            return -1;
        }

        uint32_t base = UFS_DIRECT_BLOCKS + UFS_SINGLE_INDIRECT_CAPACITY;
        uint32_t start_lb2 = (keep_blocks > base) ? (keep_blocks - base) : 0;

        int l1_changed = 0;
        int any_l1_remaining = 0;

        for (uint32_t idx1 = 0; idx1 < UFS_PTRS_PER_BLOCK; idx1++) {
            if (l1[idx1] == UFS_INVALID_BLOCK) {
                continue;
            }

            uint32_t block_start_lb2 = idx1 * UFS_PTRS_PER_BLOCK;
            uint32_t block_end_lb2 = block_start_lb2 + UFS_PTRS_PER_BLOCK;

            if (block_end_lb2 <= start_lb2) {
                /* Entirely below the keep threshold; nothing to free here. */
                any_l1_remaining = 1;
                continue;
            }

            uint32_t l2[UFS_PTRS_PER_BLOCK];
            if (ufs_read_block(l1[idx1], l2) != 0) {
                return -1;
            }

            uint32_t local_start = (start_lb2 > block_start_lb2)
                                        ? (start_lb2 - block_start_lb2)
                                        : 0;
            int l2_changed = 0;
            int any_l2_remaining = 0;

            for (uint32_t idx2 = 0; idx2 < UFS_PTRS_PER_BLOCK; idx2++) {
                if (idx2 >= local_start) {
                    if (l2[idx2] != UFS_INVALID_BLOCK) {
                        if (free_data_block(l2[idx2]) != 0) {
                            return -1;
                        }
                        l2[idx2] = UFS_INVALID_BLOCK;
                        l2_changed = 1;
                    }
                } else if (l2[idx2] != UFS_INVALID_BLOCK) {
                    any_l2_remaining = 1;
                }
            }

            if (l2_changed) {
                if (ufs_write_block(l1[idx1], l2) != 0) {
                    return -1;
                }
            }

            if (!any_l2_remaining && local_start == 0) {
                if (free_data_block(l1[idx1]) != 0) {
                    return -1;
                }
                l1[idx1] = UFS_INVALID_BLOCK;
                l1_changed = 1;
            } else {
                any_l1_remaining = 1;
            }
        }

        if (l1_changed) {
            if (ufs_write_block(inode->double_indirect, l1) != 0) {
                return -1;
            }
        }

        if (!any_l1_remaining && start_lb2 == 0) {
            if (free_data_block(inode->double_indirect) != 0) {
                return -1;
            }
            inode->double_indirect = UFS_INVALID_BLOCK;
            inode_dirty = 1;
        }
    }

    if (inode_dirty) {
        if (write_inode(inode_num, inode) != 0) {
            return -1;
        }
    }

    return 0;
}

/* =====================================================================
 * Truncate
 * ===================================================================== */

int ufs_truncate_inode(uint32_t inode_num, struct ufs_inode *inode, size_t new_size)
{
    if (inode == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (new_size > (size_t)UFS_MAX_FILE_BLOCKS * UFS_BLOCK_SIZE) {
        errno = EFBIG;
        return -1;
    }

    size_t old_size = inode->size;
    uint32_t new_blocks = (uint32_t)((new_size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE);
    uint32_t old_blocks = (uint32_t)((old_size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE);

    if (new_size > old_size) {
        /* Growing: allocate any newly needed blocks. allocate_data_block()
         * always hands back zeroed blocks, so the new range reads as
         * zero without any extra work here. */
        for (uint32_t lb = old_blocks; lb < new_blocks; lb++) {
            uint32_t phys;
            if (ensure_inode_data_block(inode_num, inode, lb, &phys) != 0) {
                /* Best-effort rollback so we don't leak the blocks we
                 * already grabbed in this loop. */
                free_inode_blocks_after(inode_num, inode, old_blocks);
                inode->size = old_size;
                write_inode(inode_num, inode);
                return -1;
            }
        }
    } else if (new_size < old_size) {
        /* Shrinking: release everything beyond the new block count. */
        if (free_inode_blocks_after(inode_num, inode, new_blocks) != 0) {
            return -1;
        }

        /* Zero the tail of the new last block so a future grow sees
         * clean bytes rather than old data. */
        if (new_size > 0 && (new_size % UFS_BLOCK_SIZE) != 0) {
            uint32_t phys;
            if (get_inode_data_block(inode, new_blocks - 1, &phys) == 0 &&
                phys != UFS_INVALID_BLOCK) {
                uint8_t buf[UFS_BLOCK_SIZE];
                if (ufs_read_block(phys, buf) == 0) {
                    size_t off = new_size % UFS_BLOCK_SIZE;
                    memset(buf + off, 0, UFS_BLOCK_SIZE - off);
                    (void)ufs_write_block(phys, buf);
                }
            }
        }
    }

    inode->size = new_size;
    inode->block_count = new_blocks;
    if (write_inode(inode_num, inode) != 0) {
        return -1;
    }

    return 0;
}

int ufs_truncate(const char *path, size_t size)
{
    struct ufs_inode inode;
    uint32_t inode_num;

    if (!g_ufs.mounted) {
        errno = ENODEV;
        return -1;
    }
    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (resolve_path(path, &inode_num) != 0) {
        return -1;
    }
    if (read_inode(inode_num, &inode) != 0) {
        return -1;
    }
    if (inode.type == UFS_TYPE_DIR) {
        errno = EISDIR;
        return -1;
    }
    if (inode.type != UFS_TYPE_FILE) {
        errno = EINVAL;
        return -1;
    }
    return ufs_truncate_inode(inode_num, &inode, size);
}
