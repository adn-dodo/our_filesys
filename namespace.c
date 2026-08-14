#include "namespace.h"
#include "ufs_internal.h"
#include "userfs_storage.h"
#include "file_io.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

static int require_mounted(void)
{
    if (!ufs_is_mounted()) {
        errno = ENODEV;
        return -1;
    }
    return 0;
}

int validate_path(const char *path)
{
    size_t length;
    size_t start;
    size_t index;

    if (path == NULL || path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    length = strlen(path);
    if (length == 0 || length > UFS_MAX_PATH) {
        errno = EINVAL;
        return -1;
    }
    if (length == 1) {
        return 0;
    }
    if (path[length - 1] == '/') {
        errno = EINVAL;
        return -1;
    }

    start = 1;
    for (index = 1; index <= length; ++index) {
        if (path[index] == '/' || path[index] == '\0') {
            size_t component_length = index - start;
            if (component_length == 0 || component_length > UFS_MAX_NAME ||
                (component_length == 1 && path[start] == '.') ||
                (component_length == 2 && path[start] == '.' &&
                 path[start + 1] == '.')) {
                errno = EINVAL;
                return -1;
            }
            start = index + 1;
        }
    }
    return 0;
}

int directory_find(uint32_t dir_inode_num, const char *name,
                   uint32_t *out_inode_num)
{
    struct ufs_disk_dirent entries[UFS_BLOCK_SIZE / UFS_DIRENT_SIZE];
    struct ufs_inode directory;
    uint32_t logical;

    if (name == NULL || out_inode_num == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (read_inode(dir_inode_num, &directory) != 0) {
        return -1;
    }
    if (directory.type != UFS_TYPE_DIR) {
        errno = ENOTDIR;
        return -1;
    }

    for (logical = 0; logical < directory.block_count; ++logical) {
        uint32_t physical;
        size_t slot;

        if (get_inode_data_block(&directory, logical, &physical) != 0 ||
            physical == UFS_INVALID_BLOCK) {
            errno = EIO;
            return -1;
        }
        if (ufs_read_block(physical, entries) != 0) {
            return -1;
        }
        for (slot = 0; slot < UFS_BLOCK_SIZE / UFS_DIRENT_SIZE; ++slot) {
            if (entries[slot].used &&
                strcmp(entries[slot].name, name) == 0) {
                *out_inode_num = entries[slot].inode_number;
                return 0;
            }
        }
    }

    errno = ENOENT;
    return -1;
}

int resolve_path(const char *path, uint32_t *out_inode_num)
{
    char component[UFS_MAX_NAME + 1];
    uint32_t current = UFS_ROOT_INODE;
    size_t position;

    if (require_mounted() != 0 || validate_path(path) != 0) {
        return -1;
    }
    if (out_inode_num == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(path, "/") == 0) {
        *out_inode_num = UFS_ROOT_INODE;
        return 0;
    }

    position = 1;
    while (path[position] != '\0') {
        size_t start = position;
        size_t component_length;
        struct ufs_inode inode;
        uint32_t next;

        while (path[position] != '/' && path[position] != '\0') {
            ++position;
        }
        component_length = position - start;
        memcpy(component, path + start, component_length);
        component[component_length] = '\0';

        if (directory_find(current, component, &next) != 0) {
            return -1;
        }
        if (read_inode(next, &inode) != 0) {
            return -1;
        }
        if (path[position] == '/' && inode.type != UFS_TYPE_DIR) {
            errno = ENOTDIR;
            return -1;
        }
        current = next;
        if (path[position] == '/') {
            ++position;
        }
    }

    *out_inode_num = current;
    return 0;
}

int resolve_parent(const char *path, uint32_t *parent_inode_num,
                   char *final_name)
{
    char parent_path[UFS_MAX_PATH + 1];
    const char *last_slash;
    size_t parent_length;

    if (require_mounted() != 0 || validate_path(path) != 0) {
        return -1;
    }
    if (parent_inode_num == NULL || final_name == NULL ||
        strcmp(path, "/") == 0) {
        errno = EINVAL;
        return -1;
    }

    last_slash = strrchr(path, '/');
    strcpy(final_name, last_slash + 1);
    parent_length = (size_t)(last_slash - path);
    if (parent_length == 0) {
        strcpy(parent_path, "/");
    } else {
        memcpy(parent_path, path, parent_length);
        parent_path[parent_length] = '\0';
    }
    return resolve_path(parent_path, parent_inode_num);
}

int directory_add(uint32_t dir_inode_num, const char *name,
                  uint32_t target_inode_num, uint32_t type)
{
    struct ufs_disk_dirent entries[UFS_BLOCK_SIZE / UFS_DIRENT_SIZE];
    struct ufs_inode directory;
    uint32_t existing;
    uint32_t logical;

    if (name == NULL || name[0] == '\0' || strlen(name) > UFS_MAX_NAME ||
        (type != UFS_TYPE_FILE && type != UFS_TYPE_DIR)) {
        errno = EINVAL;
        return -1;
    }
    if (read_inode(dir_inode_num, &directory) != 0) {
        return -1;
    }
    if (directory.type != UFS_TYPE_DIR) {
        errno = ENOTDIR;
        return -1;
    }
    if (directory_find(dir_inode_num, name, &existing) == 0) {
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT) {
        return -1;
    }

    for (logical = 0; logical < directory.block_count; ++logical) {
        uint32_t physical;
        size_t slot;

        if (get_inode_data_block(&directory, logical, &physical) != 0 ||
            physical == UFS_INVALID_BLOCK ||
            ufs_read_block(physical, entries) != 0) {
            return -1;
        }
        for (slot = 0; slot < UFS_BLOCK_SIZE / UFS_DIRENT_SIZE; ++slot) {
            if (!entries[slot].used) {
                memset(&entries[slot], 0, sizeof(entries[slot]));
                entries[slot].used = 1;
                entries[slot].inode_number = target_inode_num;
                entries[slot].type = type;
                strcpy(entries[slot].name, name);
                return ufs_write_block(physical, entries);
            }
        }
    }

    if (directory.block_count >= UFS_DIRECT_BLOCKS) {
        errno = ENOSPC;
        return -1;
    }
    {
        uint32_t physical;
        uint32_t slot = directory.block_count;

        if (allocate_data_block(&physical) != 0) {
            return -1;
        }
        memset(entries, 0, sizeof(entries));
        entries[0].used = 1;
        entries[0].inode_number = target_inode_num;
        entries[0].type = type;
        strcpy(entries[0].name, name);
        if (ufs_write_block(physical, entries) != 0) {
            (void)free_data_block(physical);
            return -1;
        }

        directory.direct[slot] = physical;
        directory.block_count++;
        directory.size = (uint64_t)directory.block_count * UFS_BLOCK_SIZE;
        if (write_inode(dir_inode_num, &directory) != 0) {
            directory.direct[slot] = UFS_INVALID_BLOCK;
            (void)free_data_block(physical);
            return -1;
        }
    }
    return 0;
}

int directory_remove(uint32_t dir_inode_num, const char *name)
{
    struct ufs_disk_dirent entries[UFS_BLOCK_SIZE / UFS_DIRENT_SIZE];
    struct ufs_inode directory;
    uint32_t logical;

    if (name == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (read_inode(dir_inode_num, &directory) != 0) {
        return -1;
    }
    if (directory.type != UFS_TYPE_DIR) {
        errno = ENOTDIR;
        return -1;
    }

    for (logical = 0; logical < directory.block_count; ++logical) {
        uint32_t physical;
        size_t slot;

        if (get_inode_data_block(&directory, logical, &physical) != 0 ||
            physical == UFS_INVALID_BLOCK ||
            ufs_read_block(physical, entries) != 0) {
            return -1;
        }
        for (slot = 0; slot < UFS_BLOCK_SIZE / UFS_DIRENT_SIZE; ++slot) {
            if (entries[slot].used && strcmp(entries[slot].name, name) == 0) {
                memset(&entries[slot], 0, sizeof(entries[slot]));
                return ufs_write_block(physical, entries);
            }
        }
    }
    errno = ENOENT;
    return -1;
}

int directory_is_empty(uint32_t dir_inode_num)
{
    struct ufs_disk_dirent entries[UFS_BLOCK_SIZE / UFS_DIRENT_SIZE];
    struct ufs_inode directory;
    uint32_t logical;

    if (read_inode(dir_inode_num, &directory) != 0) {
        return -1;
    }
    if (directory.type != UFS_TYPE_DIR) {
        errno = ENOTDIR;
        return -1;
    }
    for (logical = 0; logical < directory.block_count; ++logical) {
        uint32_t physical;
        size_t slot;

        if (get_inode_data_block(&directory, logical, &physical) != 0 ||
            physical == UFS_INVALID_BLOCK ||
            ufs_read_block(physical, entries) != 0) {
            return -1;
        }
        for (slot = 0; slot < UFS_BLOCK_SIZE / UFS_DIRENT_SIZE; ++slot) {
            if (entries[slot].used) {
                return 0;
            }
        }
    }
    return 1;
}

static int create_node(const char *path, uint32_t type)
{
    struct ufs_inode inode;
    char name[UFS_MAX_NAME + 1];
    uint32_t parent;
    uint32_t existing;
    uint32_t inode_num;

    if (resolve_parent(path, &parent, name) != 0) {
        return -1;
    }
    if (directory_find(parent, name, &existing) == 0) {
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT) {
        return -1;
    }
    if (allocate_inode(&inode_num) != 0) {
        return -1;
    }

    ufs_init_inode(&inode, type);
    if (write_inode(inode_num, &inode) != 0 ||
        directory_add(parent, name, inode_num, type) != 0) {
        int saved_errno = errno;
        (void)free_inode(inode_num);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int ufs_mkdir(const char *path)
{
    return create_node(path, UFS_TYPE_DIR);
}

int ufs_create(const char *path)
{
    return create_node(path, UFS_TYPE_FILE);
}

int ufs_rmdir(const char *path)
{
    struct ufs_inode target;
    char name[UFS_MAX_NAME + 1];
    uint32_t parent;
    uint32_t target_num;
    int empty;

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(path, "/") == 0) {
        errno = EBUSY;
        return -1;
    }
    if (resolve_parent(path, &parent, name) != 0 ||
        directory_find(parent, name, &target_num) != 0 ||
        read_inode(target_num, &target) != 0) {
        return -1;
    }
    if (target.type != UFS_TYPE_DIR) {
        errno = ENOTDIR;
        return -1;
    }
    empty = directory_is_empty(target_num);
    if (empty < 0) {
        return -1;
    }
    if (!empty) {
        errno = ENOTEMPTY;
        return -1;
    }
    if (directory_remove(parent, name) != 0) {
        return -1;
    }
    if (free_inode(target_num) != 0) {
        int saved_errno = errno;
        (void)directory_add(parent, name, target_num, UFS_TYPE_DIR);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int ufs_unlink(const char *path)
{
    struct ufs_inode target;
    char name[UFS_MAX_NAME + 1];
    uint32_t parent;
    uint32_t target_num;

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(path, "/") == 0) {
        errno = EISDIR;
        return -1;
    }
    if (resolve_parent(path, &parent, name) != 0 ||
        directory_find(parent, name, &target_num) != 0 ||
        read_inode(target_num, &target) != 0) {
        return -1;
    }
    if (target.type == UFS_TYPE_DIR) {
        errno = EISDIR;
        return -1;
    }
    if (target.type != UFS_TYPE_FILE) {
        errno = EINVAL;
        return -1;
    }
    if (inode_is_open(target_num)) {
        errno = EBUSY;
        return -1;
    }
    if (directory_remove(parent, name) != 0) {
        return -1;
    }
    if (free_inode(target_num) != 0) {
        int saved_errno = errno;
        (void)directory_add(parent, name, target_num, UFS_TYPE_FILE);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int ufs_listdir(const char *path, struct ufs_dirent *entries,
                size_t max_entries)
{
    struct ufs_disk_dirent disk_entries[UFS_BLOCK_SIZE / UFS_DIRENT_SIZE];
    struct ufs_inode directory;
    uint32_t directory_num;
    uint32_t logical;
    size_t copied = 0;

    if (max_entries > 0 && entries == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (resolve_path(path, &directory_num) != 0 ||
        read_inode(directory_num, &directory) != 0) {
        return -1;
    }
    if (directory.type != UFS_TYPE_DIR) {
        errno = ENOTDIR;
        return -1;
    }

    for (logical = 0; logical < directory.block_count && copied < max_entries;
         ++logical) {
        uint32_t physical;
        size_t slot;

        if (get_inode_data_block(&directory, logical, &physical) != 0 ||
            physical == UFS_INVALID_BLOCK ||
            ufs_read_block(physical, disk_entries) != 0) {
            return -1;
        }
        for (slot = 0; slot < UFS_BLOCK_SIZE / UFS_DIRENT_SIZE &&
                       copied < max_entries; ++slot) {
            if (disk_entries[slot].used) {
                struct ufs_inode item;
                if (read_inode(disk_entries[slot].inode_number, &item) != 0) {
                    return -1;
                }
                strcpy(entries[copied].name, disk_entries[slot].name);
                entries[copied].type = (int)item.type;
                entries[copied].size = (size_t)item.size;
                ++copied;
            }
        }
    }
    return (int)copied;
}

int ufs_stat(const char *path, struct ufs_stat *st)
{
    struct ufs_inode inode;
    uint32_t inode_num;

    if (st == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (resolve_path(path, &inode_num) != 0 ||
        read_inode(inode_num, &inode) != 0) {
        return -1;
    }
    st->type = (int)inode.type;
    st->size = (size_t)inode.size;
    return 0;
}
