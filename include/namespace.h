#ifndef NAMESPACE_H
#define NAMESPACE_H

#include "userfs.h"
#include <stdint.h>

#define UFS_RESOLVE_FOLLOW_FINAL    0x01
#define UFS_RESOLVE_NOFOLLOW_FINAL  0x02
#define UFS_MAX_SYMLINK_DEPTH       40

// ---------------------------------------------------------
// Path Validation & Resolution
// ---------------------------------------------------------
int validate_path(const char *path);
int resolve_path(const char *path, uint32_t *out_inode_num);
int resolve_path_ex(const char *path, int flags, uint32_t *out_inode_num);
int resolve_parent(const char *path, uint32_t *parent_inode_num, char *final_name);

// ---------------------------------------------------------
// Directory Helpers
// ---------------------------------------------------------
int directory_find(uint32_t dir_inode_num, const char *name, uint32_t *out_inode_num);
// Updated to accept 'type' parameter required by Member 1's dirent structure
int directory_add(uint32_t dir_inode_num, const char *name, uint32_t target_inode_num, uint32_t type);
int directory_remove(uint32_t dir_inode_num, const char *name);
int directory_is_empty(uint32_t dir_inode_num);

#endif // NAMESPACE_H
