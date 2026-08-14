#ifndef FILE_IO_H
#define FILE_IO_H

#include <stdint.h>

/* Lifecycle code calls this after mount and during unmount. */
void descriptor_table_reset(void);

/* Namespace code calls this before unlinking a file. */
int inode_is_open(uint32_t inode_number);

#endif
