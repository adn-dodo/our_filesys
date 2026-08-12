#ifndef FILE_IO_H
#define FILE_IO_H

#include <stdint.h>

/*
 * Reset all temporary UserFS file descriptors.
 * Member 1 should call this during mount and unmount.
 */
void descriptor_table_reset(void);

/*
 * Return 1 if inode_number is currently open, otherwise return 0.
 * Member 3 uses this to prevent deletion of an open file.
 */
int inode_is_open(uint32_t inode_number);

#endif
