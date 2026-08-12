#include "namespace.h"
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

/* =========================================================================
 * STATEFUL MOCKS (RAM Disk for Local Testing)
 * Remove these once your teammates integrate their code!
 * ========================================================================= */
static struct ufs_inode fake_inode_table[256];
static uint8_t fake_data_blocks[2048][UFS_BLOCK_SIZE];
static uint32_t fake_free_inode = 1; // 0 is root
static uint32_t fake_free_block = 35; // Data region starts at 35

int read_inode(uint32_t inode_num, struct ufs_inode *inode) { 
    // Auto-initialize the root directory on the first read
    if (inode_num == UFS_ROOT_INODE && fake_inode_table[0].type == 0) {
        fake_inode_table[0].type = UFS_TYPE_DIR;
        fake_inode_table[0].size = 0; 
    }
    *inode = fake_inode_table[inode_num]; 
    return 0; 
}

int write_inode(uint32_t inode_num, struct ufs_inode *inode) { 
    fake_inode_table[inode_num] = *inode; 
    return 0; 
}

int allocate_inode(uint32_t *inode_num) { 
    *inode_num = fake_free_inode++; 
    return 0; 
}

int free_inode(uint32_t inode_num) { 
    (void)inode_num; // We won't bother reclaiming memory in the mock
    return 0; 
}

int read_block(uint32_t block_num, void *buffer) { 
    memcpy(buffer, fake_data_blocks[block_num], UFS_BLOCK_SIZE); 
    return 0; 
}

int write_block(uint32_t block_num, const void *buffer) { 
    memcpy(fake_data_blocks[block_num], buffer, UFS_BLOCK_SIZE); 
    return 0; 
}

int allocate_data_block(uint32_t *block_num) { 
    *block_num = fake_free_block++; 
    return 0; 
}

int free_data_block(uint32_t block_num) { 
    (void)block_num; 
    return 0; 
}

int inode_is_open(uint32_t inode_num) { 
    (void)inode_num; 
    return 0; 
}

/* =========================================================================
 * PATH VALIDATION & RESOLUTION
 * ========================================================================= */

int validate_path(const char *path) {
    if (!path || path[0] != '/') return -EINVAL; 
    if (strlen(path) > UFS_MAX_PATH) return -EINVAL; 
    if (strstr(path, "..") != NULL) return -EINVAL;  

    char temp[UFS_MAX_PATH + 1];
    strncpy(temp, path, UFS_MAX_PATH);
    temp[UFS_MAX_PATH] = '\0';

    char *token = strtok(temp, "/");
    while (token != NULL) {
        if (strlen(token) > UFS_MAX_NAME) return -EINVAL; 
        token = strtok(NULL, "/");
    }
    return 0;
}

int resolve_parent(const char *path, uint32_t *parent_inode_num, char *final_name) {
    int val = validate_path(path);
    if (val < 0) return val;

    if (strcmp(path, "/") == 0) return -EINVAL; 

    const char *last_slash = strrchr(path, '/');
    if (*(last_slash + 1) == '\0') return -EINVAL; 

    strncpy(final_name, last_slash + 1, UFS_MAX_NAME);
    final_name[UFS_MAX_NAME] = '\0';

    char parent_path[UFS_MAX_PATH + 1];
    size_t parent_len = (size_t)(last_slash - path);
    
    if (parent_len == 0) {
        strcpy(parent_path, "/");
    } else {
        strncpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
    }

    return resolve_path(parent_path, parent_inode_num);
}

int resolve_path(const char *path, uint32_t *out_inode_num) {
    int val = validate_path(path);
    if (val < 0) return val;

    uint32_t current_inode = UFS_ROOT_INODE; 
    
    if (strcmp(path, "/") == 0) {
        *out_inode_num = current_inode;
        return 0;
    }

    char temp[UFS_MAX_PATH + 1];
    strncpy(temp, path, UFS_MAX_PATH);
    temp[UFS_MAX_PATH] = '\0';

    char *token = strtok(temp, "/");
    while (token != NULL) {
        uint32_t next_inode;
        int res = directory_find(current_inode, token, &next_inode);
        if (res < 0) return res; 
        
        struct ufs_inode inode_data;
        read_inode(next_inode, &inode_data);
        
        char *peek = strtok(NULL, "/");
        if (peek != NULL && inode_data.type != UFS_TYPE_DIR) {
            return -ENOTDIR;
        }
        
        current_inode = next_inode;
        token = peek;
    }

    *out_inode_num = current_inode;
    return 0;
}

/* =========================================================================
 * DIRECTORY ENTRY HELPERS
 * ========================================================================= */

int directory_find(uint32_t dir_inode_num, const char *name, uint32_t *out_inode_num) {
    struct ufs_inode dir;
    read_inode(dir_inode_num, &dir);
    
    if (dir.type != UFS_TYPE_DIR) return -ENOTDIR;

    size_t dirents_per_block = UFS_BLOCK_SIZE / sizeof(struct ufs_disk_dirent);
    struct ufs_disk_dirent buffer[dirents_per_block];
    uint32_t blocks = (uint32_t)(dir.size / UFS_BLOCK_SIZE);

    for (uint32_t i = 0; i < blocks; i++) {
        read_block(dir.direct[i], buffer);
        for (size_t j = 0; j < dirents_per_block; j++) {
            // Using Member 1's 'used' and 'inode_number' fields
            if (buffer[j].used && strcmp(buffer[j].name, name) == 0) {
                *out_inode_num = buffer[j].inode_number;
                return 0;
            }
        }
    }
    return -ENOENT;
}

int directory_add(uint32_t dir_inode_num, const char *name, uint32_t target_inode_num, uint32_t type) {
    struct ufs_inode dir;
    read_inode(dir_inode_num, &dir);

    size_t dirents_per_block = UFS_BLOCK_SIZE / sizeof(struct ufs_disk_dirent);
    struct ufs_disk_dirent buffer[dirents_per_block];
    uint32_t blocks = (uint32_t)(dir.size / UFS_BLOCK_SIZE);

    for (uint32_t i = 0; i < blocks; i++) {
        read_block(dir.direct[i], buffer);
        for (size_t j = 0; j < dirents_per_block; j++) {
            if (!buffer[j].used) {
                buffer[j].used = 1;
                buffer[j].inode_number = target_inode_num;
                buffer[j].type = type;
                strncpy(buffer[j].name, name, UFS_MAX_NAME);
                buffer[j].name[UFS_MAX_NAME] = '\0';
                write_block(dir.direct[i], buffer);
                return 0;
            }
        }
    }

    if (blocks >= UFS_DIRECT_BLOCKS) return -ENOSPC;

    uint32_t new_block;
    if (allocate_data_block(&new_block) < 0) return -ENOSPC;

    memset(buffer, 0, UFS_BLOCK_SIZE);
    buffer[0].used = 1;
    buffer[0].inode_number = target_inode_num;
    buffer[0].type = type;
    strncpy(buffer[0].name, name, UFS_MAX_NAME);
    buffer[0].name[UFS_MAX_NAME] = '\0';
    
    write_block(new_block, buffer);
    dir.direct[blocks] = new_block;
    dir.size += UFS_BLOCK_SIZE;
    write_inode(dir_inode_num, &dir);

    return 0;
}

int directory_remove(uint32_t dir_inode_num, const char *name) {
    struct ufs_inode dir;
    read_inode(dir_inode_num, &dir);

    size_t dirents_per_block = UFS_BLOCK_SIZE / sizeof(struct ufs_disk_dirent);
    struct ufs_disk_dirent buffer[dirents_per_block];
    uint32_t blocks = (uint32_t)(dir.size / UFS_BLOCK_SIZE);

    for (uint32_t i = 0; i < blocks; i++) {
        read_block(dir.direct[i], buffer);
        for (size_t j = 0; j < dirents_per_block; j++) {
            if (buffer[j].used && strcmp(buffer[j].name, name) == 0) {
                buffer[j].used = 0; 
                buffer[j].inode_number = 0;
                memset(buffer[j].name, 0, sizeof(buffer[j].name));
                write_block(dir.direct[i], buffer);
                return 0;
            }
        }
    }
    return -ENOENT;
}

int directory_is_empty(uint32_t dir_inode_num) {
    struct ufs_inode dir;
    read_inode(dir_inode_num, &dir);

    size_t dirents_per_block = UFS_BLOCK_SIZE / sizeof(struct ufs_disk_dirent);
    struct ufs_disk_dirent buffer[dirents_per_block];
    uint32_t blocks = (uint32_t)(dir.size / UFS_BLOCK_SIZE);

    for (uint32_t i = 0; i < blocks; i++) {
        read_block(dir.direct[i], buffer);
        for (size_t j = 0; j < dirents_per_block; j++) {
            if (buffer[j].used) {
                return 0; // Found a used slot, not empty
            }
        }
    }
    return 1; 
}

/* =========================================================================
 * PUBLIC API OVERRIDES
 * ========================================================================= */

int ufs_mkdir(const char *path) {
    uint32_t parent_inode;
    char name[UFS_MAX_NAME + 1];
    
    int res = resolve_parent(path, &parent_inode, name);
    if (res < 0) { errno = -res; return -1; }

    uint32_t existing;
    if (directory_find(parent_inode, name, &existing) == 0) {
        errno = EEXIST;
        return -1;
    }

    uint32_t new_inode;
    if (allocate_inode(&new_inode) < 0) {
        errno = ENOSPC;
        return -1;
    }

    struct ufs_inode dir = {0};
    dir.type = UFS_TYPE_DIR;
    dir.size = 0;
    write_inode(new_inode, &dir);

    // Pass UFS_TYPE_DIR to our updated directory_add signature
    res = directory_add(parent_inode, name, new_inode, UFS_TYPE_DIR);
    if (res < 0) {
        free_inode(new_inode);
        errno = -res;
        return -1;
    }
    return 0;
}

int ufs_rmdir(const char *path) {
    if (strcmp(path, "/") == 0) {
        errno = EINVAL;
        return -1;
    }

    uint32_t parent_inode, target_inode;
    char name[UFS_MAX_NAME + 1];

    int res = resolve_parent(path, &parent_inode, name);
    if (res < 0) { errno = -res; return -1; }

    res = directory_find(parent_inode, name, &target_inode);
    if (res < 0) { errno = -res; return -1; }

    struct ufs_inode target;
    read_inode(target_inode, &target);

    if (target.type != UFS_TYPE_DIR) {
        errno = ENOTDIR;
        return -1;
    }

    if (!directory_is_empty(target_inode)) {
        errno = ENOTEMPTY;
        return -1;
    }

    directory_remove(parent_inode, name);
    free_inode(target_inode);
    
    return 0;
}

int ufs_create(const char *path) {
    uint32_t parent_inode;
    char name[UFS_MAX_NAME + 1];
    
    int res = resolve_parent(path, &parent_inode, name);
    if (res < 0) { errno = -res; return -1; }

    uint32_t existing;
    if (directory_find(parent_inode, name, &existing) == 0) {
        errno = EEXIST;
        return -1;
    }

    uint32_t new_inode;
    if (allocate_inode(&new_inode) < 0) {
        errno = ENOSPC;
        return -1;
    }

    struct ufs_inode file = {0};
    file.type = UFS_TYPE_FILE;
    file.size = 0;
    write_inode(new_inode, &file);

    // Pass UFS_TYPE_FILE to our updated directory_add signature
    res = directory_add(parent_inode, name, new_inode, UFS_TYPE_FILE);
    if (res < 0) {
        free_inode(new_inode);
        errno = -res;
        return -1;
    }
    return 0;
}

int ufs_unlink(const char *path) {
    if (strcmp(path, "/") == 0) {
        errno = EINVAL;
        return -1;
    }

    uint32_t parent_inode, target_inode;
    char name[UFS_MAX_NAME + 1];

    int res = resolve_parent(path, &parent_inode, name);
    if (res < 0) { errno = -res; return -1; }

    res = directory_find(parent_inode, name, &target_inode);
    if (res < 0) { errno = -res; return -1; }

    struct ufs_inode target;
    read_inode(target_inode, &target);

    if (target.type == UFS_TYPE_DIR) {
        errno = EISDIR;
        return -1;
    }

    if (inode_is_open(target_inode)) {
        errno = EBUSY; 
        return -1;
    }

    directory_remove(parent_inode, name);
    free_inode(target_inode);
    return 0;
}

int ufs_listdir(const char *path, struct ufs_dirent *entries, size_t max_entries) {
    uint32_t dir_inode_num;
    int res = resolve_path(path, &dir_inode_num);
    if (res < 0) { errno = -res; return -1; }

    struct ufs_inode dir;
    read_inode(dir_inode_num, &dir);

    if (dir.type != UFS_TYPE_DIR) {
        errno = ENOTDIR;
        return -1;
    }

    size_t dirents_per_block = UFS_BLOCK_SIZE / sizeof(struct ufs_disk_dirent);
    struct ufs_disk_dirent buffer[dirents_per_block];
    uint32_t blocks = (uint32_t)(dir.size / UFS_BLOCK_SIZE);
    size_t count = 0;

    for (uint32_t i = 0; i < blocks; i++) {
        read_block(dir.direct[i], buffer);
        for (size_t j = 0; j < dirents_per_block; j++) {
            if (buffer[j].used && count < max_entries) {
                struct ufs_inode item;
                read_inode(buffer[j].inode_number, &item);
                
                strncpy(entries[count].name, buffer[j].name, UFS_MAX_NAME);
                entries[count].name[UFS_MAX_NAME] = '\0';
                entries[count].type = item.type;
                entries[count].size = item.size;
                count++;
            }
        }
    }
    return (int)count; 
}

int ufs_stat(const char *path, struct ufs_stat *st) {
    uint32_t target_inode;
    int res = resolve_path(path, &target_inode);
    if (res < 0) { errno = -res; return -1; }

    struct ufs_inode inode_data;
    read_inode(target_inode, &inode_data);

    st->type = inode_data.type;
    st->size = inode_data.size;
    
    return 0;
}
