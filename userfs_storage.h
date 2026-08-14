#ifndef USERFS_STORAGE_H
#define USERFS_STORAGE_H

#include <stdint.h>
#include <stddef.h>

/* 
 * Include the shared internal header to ensure struct ufs_inode 
 * and other global layout constants are defined.
 */
#include "ufs_internal.h"

/* =====================================================================
 * Bit operations
 * ===================================================================== */
int bitmap_test(const uint8_t *bitmap, uint32_t bit);
void bitmap_set(uint8_t *bitmap, uint32_t bit);
void bitmap_clear(uint8_t *bitmap, uint32_t bit);

/* =====================================================================
 * Bitmap block I/O
 * ===================================================================== */
int read_inode_bitmap(uint8_t *bitmap);
int write_inode_bitmap(const uint8_t *bitmap);
int read_block_bitmap(uint8_t *bitmap);
int write_block_bitmap(const uint8_t *bitmap);

/* =====================================================================
 * Inode table read / write
 * ===================================================================== */
int read_inode(uint32_t inode_num, struct ufs_inode *inode);
int write_inode(uint32_t inode_num, const struct ufs_inode *inode);

/* =====================================================================
 * Inode allocation / release
 * ===================================================================== */
int allocate_inode(uint32_t *inode_num_out);
int free_inode(uint32_t inode_num);

/* =====================================================================
 * Data block allocation / release
 * ===================================================================== */
int zero_block(uint32_t block_num);
int allocate_data_block(uint32_t *block_num_out);
int free_data_block(uint32_t block_num);

/* =====================================================================
 * Logical block -> physical block translation
 * ===================================================================== */
int get_inode_data_block(const struct ufs_inode *inode, uint32_t logical_block,
                         uint32_t *phys_block_out);
int ensure_inode_data_block(uint32_t inode_num, struct ufs_inode *inode,
                            uint32_t logical_block, uint32_t *phys_block_out);
int free_inode_blocks_after(uint32_t inode_num, struct ufs_inode *inode,
                            uint32_t keep_blocks);

/* =====================================================================
 * Truncate
 * ===================================================================== */
int ufs_truncate_inode(uint32_t inode_num, struct ufs_inode *inode, size_t new_size);

#endif /* USERFS_STORAGE_H */
