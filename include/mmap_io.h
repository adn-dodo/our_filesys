#ifndef MMAP_IO_H
#define MMAP_IO_H

#include <stdint.h>

/* Lifecycle and namespace integration helpers. */
int ufs_mmap_has_active(void);
int ufs_mmap_inode_is_mapped(uint32_t inode_number);

#endif
