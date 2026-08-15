#ifndef FILE_IO_H
#define FILE_IO_H

#include <stdint.h>

struct file_io_fd_info {
    uint32_t inode_number;
    uint32_t inode_generation;
    int flags;
};

/* Lifecycle code calls this after mount and during unmount. */
void descriptor_table_reset(void);

/* Namespace code calls this before unlinking a file. */
int inode_is_open(uint32_t inode_number);

/* mmap_io uses this snapshot instead of accessing the private descriptor
 * table directly. */
int file_io_get_fd_info(int fd, struct file_io_fd_info *info);

#endif
