#ifndef FILE_IO_H
#define FILE_IO_H

#include <stdint.h>

void descriptor_table_reset(void);
int inode_is_open(uint32_t inode_number);

#endif
