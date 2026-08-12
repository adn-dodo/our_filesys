#ifndef FILE_IO_H
#define FILE_IO_H

#include "userfs.h"

#include <stdint.h>

/*
 * Member 4's private view of the Version 1 inode.
 * This makes file_io.c independent of ufs_internal.h.
 */
#define FILE_IO_DIRECT_BLOCKS 8U
#define FILE_IO_INVALID_BLOCK UINT32_MAX
#define FILE_IO_DATA_START    35U

typedef struct file_io_inode {
    uint32_t type;
    uint32_t flags;
    uint64_t size;
    uint32_t block_count;
    uint32_t direct[FILE_IO_DIRECT_BLOCKS];
    uint32_t single_indirect;
    uint32_t double_indirect;
    uint32_t reserved;
} file_io_inode_t;

_Static_assert(sizeof(file_io_inode_t) == 64U,
               "Member 4 inode view must be 64 bytes");

/*
 * Integration helpers supplied by Members 1-3.
 * They return 0 on success or a negative errno value on failure.
 */
int ufs_is_mounted(void);
int resolve_path(const char *path, uint32_t *inode_number);
int read_inode_for_io(uint32_t inode_number, file_io_inode_t *inode);
int read_block_for_io(uint32_t block_number, void *buffer);
int write_block_for_io(uint32_t block_number, const void *buffer);
int get_inode_data_block_for_io(const file_io_inode_t *inode,
                                uint32_t logical_block,
                                uint32_t *physical_block);
int truncate_inode_for_io(uint32_t inode_number,
                          file_io_inode_t *inode,
                          uint64_t new_size);

/* Member 1 calls this after mount and during unmount. */
void descriptor_table_reset(void);

/* Member 3 calls this before unlinking a file. */
int inode_is_open(uint32_t inode_number);

#endif
