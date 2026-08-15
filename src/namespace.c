#include "namespace.h"
#include "ufs_internal.h"
#include "userfs_storage.h"
#include "file_io.h"
#include "metadata.h"
#include "mmap_io.h"
#include "ufs_sync.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

/* Normalize an absolute path produced while expanding a symbolic link.
 * Public UserFS paths remain strict, but a symlink target may contain . or ... */
static int normalize_absolute_path(const char *input, char *output)
{
    size_t component_starts[UFS_MAX_PATH + 1];
    size_t depth = 0;
    size_t input_pos = 0;
    size_t output_len = 1;

    if (input == NULL || output == NULL || input[0] != '/') {
        errno = EINVAL;
        return -1;
    }

    output[0] = '/';
    output[1] = '\0';

    while (input[input_pos] != '\0') {
        size_t start;
        size_t length;

        while (input[input_pos] == '/') {
            ++input_pos;
        }
        if (input[input_pos] == '\0') {
            break;
        }
        start = input_pos;
        while (input[input_pos] != '/' && input[input_pos] != '\0') {
            ++input_pos;
        }
        length = input_pos - start;

        if (length == 1 && input[start] == '.') {
            continue;
        }
        if (length == 2 && input[start] == '.' && input[start + 1] == '.') {
            if (depth > 0) {
                output_len = component_starts[--depth];
                output[output_len] = '\0';
            }
            continue;
        }
        if (length > UFS_MAX_NAME) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (output_len + (output_len > 1 ? 1U : 0U) + length > UFS_MAX_PATH) {
            errno = ENAMETOOLONG;
            return -1;
        }

        component_starts[depth++] = output_len;
        if (output_len > 1) {
            output[output_len++] = '/';
        }
        memcpy(output + output_len, input + start, length);
        output_len += length;
        output[output_len] = '\0';
    }
    return 0;
}

static int read_symlink_target(const struct ufs_inode *inode,
                               char target[UFS_MAX_PATH + 1])
{
    uint8_t block[UFS_BLOCK_SIZE];
    uint32_t physical;

    if (inode == NULL || inode->type != UFS_TYPE_SYMLINK ||
        inode->size == 0 || inode->size > UFS_MAX_PATH ||
        inode->block_count == 0 ||
        get_inode_data_block(inode, 0, &physical) != 0 ||
        physical == UFS_INVALID_BLOCK) {
        errno = EIO;
        return -1;
    }
    if (ufs_read_block(physical, block) != 0) {
        return -1;
    }
    memcpy(target, block, (size_t)inode->size);
    target[inode->size] = '\0';
    return 0;
}

static int resolve_absolute(const char *path, int flags,
                            uint32_t *out_inode_num, unsigned int depth)
{
    uint32_t current = UFS_ROOT_INODE;
    size_t position = 1;
    char parent_path[UFS_MAX_PATH + 1] = "/";

    if (depth >= UFS_MAX_SYMLINK_DEPTH) {
        errno = ELOOP;
        return -1;
    }
    if (strcmp(path, "/") == 0) {
        *out_inode_num = UFS_ROOT_INODE;
        return 0;
    }

    while (path[position] != '\0') {
        char component[UFS_MAX_NAME + 1];
        size_t start = position;
        size_t component_length;
        uint32_t next;
        struct ufs_inode inode;
        struct ufs_inode current_inode;
        int is_final;

        if (read_inode(current, &current_inode) != 0) {
            return -1;
        }
        if (ufs_check_access_inode(&current_inode, X_OK) != 0) {
            return -1;
        }

        while (path[position] != '/' && path[position] != '\0') {
            ++position;
        }
        component_length = position - start;
        if (component_length == 0 || component_length > UFS_MAX_NAME) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(component, path + start, component_length);
        component[component_length] = '\0';

        if (directory_find(current, component, &next) != 0 ||
            read_inode(next, &inode) != 0) {
            return -1;
        }
        is_final = path[position] == '\0';

        if (inode.type == UFS_TYPE_SYMLINK &&
            !(is_final && (flags & UFS_RESOLVE_NOFOLLOW_FINAL))) {
            char target[UFS_MAX_PATH + 1];
            char combined[(UFS_MAX_PATH * 2) + 3];
            char normalized[UFS_MAX_PATH + 1];
            const char *remainder = path + position;
            int written;

            if (read_symlink_target(&inode, target) != 0) {
                return -1;
            }
            if (target[0] == '/') {
                written = snprintf(combined, sizeof(combined), "%s%s",
                                   target, remainder);
            } else if (strcmp(parent_path, "/") == 0) {
                written = snprintf(combined, sizeof(combined), "/%s%s",
                                   target, remainder);
            } else {
                written = snprintf(combined, sizeof(combined), "%s/%s%s",
                                   parent_path, target, remainder);
            }
            if (written < 0 || (size_t)written >= sizeof(combined)) {
                errno = ENAMETOOLONG;
                return -1;
            }
            if (normalize_absolute_path(combined, normalized) != 0) {
                return -1;
            }
            return resolve_absolute(normalized, flags, out_inode_num, depth + 1U);
        }

        if (!is_final && inode.type != UFS_TYPE_DIR) {
            errno = ENOTDIR;
            return -1;
        }
        current = next;

        if (!is_final) {
            size_t old_length = strlen(parent_path);
            if (old_length + (old_length > 1 ? 1U : 0U) + component_length >
                UFS_MAX_PATH) {
                errno = ENAMETOOLONG;
                return -1;
            }
            if (old_length > 1) {
                strcat(parent_path, "/");
            }
            strcat(parent_path, component);
            ++position;
        }
    }

    *out_inode_num = current;
    return 0;
}

int resolve_path(const char *path, uint32_t *out_inode_num)
{
    return resolve_path_ex(path, UFS_RESOLVE_FOLLOW_FINAL, out_inode_num);
}

int resolve_path_ex(const char *path, int flags, uint32_t *out_inode_num)
{
    if (flags != 0 && flags != UFS_RESOLVE_FOLLOW_FINAL &&
        flags != UFS_RESOLVE_NOFOLLOW_FINAL) {
        errno = EINVAL;
        return -1;
    }

    if (require_mounted() != 0 || validate_path(path) != 0) {
        return -1;
    }
    if (out_inode_num == NULL) {
        errno = EINVAL;
        return -1;
    }
    return resolve_absolute(path, flags, out_inode_num, 0);
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
        (type != UFS_TYPE_FILE && type != UFS_TYPE_DIR &&
         type != UFS_TYPE_SYMLINK)) {
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
    struct ufs_inode parent_inode;
    char name[UFS_MAX_NAME + 1];
    uint32_t parent;
    uint32_t existing;
    uint32_t inode_num;
    uint32_t generation;

    if (resolve_parent(path, &parent, name) != 0 ||
        read_inode(parent, &parent_inode) != 0) {
        return -1;
    }
    if (ufs_check_access_inode(&parent_inode, W_OK | X_OK) != 0) {
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

    if (read_inode(inode_num, &inode) != 0) {
        int saved_errno = errno;
        (void)free_inode(inode_num);
        errno = saved_errno;
        return -1;
    }
    generation = inode.generation;
    ufs_init_inode(&inode, type);
    inode.generation = generation;
    if (ufs_initialize_metadata(&inode, type, 0) != 0) {
        int saved_errno = errno;
        (void)free_inode(inode_num);
        errno = saved_errno;
        return -1;
    }
    if (write_inode(inode_num, &inode) != 0 ||
        directory_add(parent, name, inode_num, type) != 0) {
        int saved_errno = errno;
        (void)free_inode(inode_num);
        errno = saved_errno;
        return -1;
    }
    if (read_inode(parent, &parent_inode) != 0) {
        return -1;
    }
    ufs_touch_mtime_ctime(&parent_inode);
    if (write_inode(parent, &parent_inode) != 0) {
        return -1;
    }
    return 0;
}

static int ufs_mkdir_unlocked(const char *path)
{
    return create_node(path, UFS_TYPE_DIR);
}

static int ufs_create_unlocked(const char *path)
{
    return create_node(path, UFS_TYPE_FILE);
}

static int ufs_rmdir_unlocked(const char *path)
{
    struct ufs_inode target;
    struct ufs_inode parent_inode;
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
        read_inode(parent, &parent_inode) != 0) {
        return -1;
    }
    if (ufs_check_access_inode(&parent_inode, W_OK | X_OK) != 0) {
        return -1;
    }
    if (
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
    if (read_inode(parent, &parent_inode) != 0) {
        return -1;
    }
    ufs_touch_mtime_ctime(&parent_inode);
    if (write_inode(parent, &parent_inode) != 0) {
        return -1;
    }
    return 0;
}

static int ufs_unlink_unlocked(const char *path)
{
    struct ufs_inode target;
    struct ufs_inode parent_inode;
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
        read_inode(parent, &parent_inode) != 0) {
        return -1;
    }
    if (ufs_check_access_inode(&parent_inode, W_OK | X_OK) != 0) {
        return -1;
    }
    if (
        directory_find(parent, name, &target_num) != 0 ||
        read_inode(target_num, &target) != 0) {
        return -1;
    }
    if (target.type == UFS_TYPE_DIR) {
        errno = EISDIR;
        return -1;
    }
    if (target.type != UFS_TYPE_FILE && target.type != UFS_TYPE_SYMLINK) {
        errno = EINVAL;
        return -1;
    }
    if (inode_is_open(target_num) || ufs_mmap_inode_is_mapped(target_num)) {
        errno = EBUSY;
        return -1;
    }
    if (directory_remove(parent, name) != 0) {
        return -1;
    }
    if (target.link_count == 0) {
        int saved_errno = EIO;
        (void)directory_add(parent, name, target_num, target.type);
        errno = saved_errno;
        return -1;
    }
    --target.link_count;
    ufs_touch_ctime(&target);
    if (target.link_count == 0) {
        if (free_inode(target_num) != 0) {
            int saved_errno = errno;
            (void)directory_add(parent, name, target_num, target.type);
            errno = saved_errno;
            return -1;
        }
    } else if (write_inode(target_num, &target) != 0) {
        int saved_errno = errno;
        (void)directory_add(parent, name, target_num, target.type);
        errno = saved_errno;
        return -1;
    }
    if (read_inode(parent, &parent_inode) != 0) {
        return -1;
    }
    ufs_touch_mtime_ctime(&parent_inode);
    if (write_inode(parent, &parent_inode) != 0) {
        return -1;
    }
    return 0;
}

static int ufs_listdir_unlocked(const char *path,
                                struct ufs_dirent *entries,
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
    if (ufs_check_access_inode(&directory, R_OK | X_OK) != 0) {
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

static int ufs_stat_unlocked(const char *path, struct ufs_stat *st)
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
    return ufs_fill_stat_from_inode(&inode, st);
}

static int ufs_lstat_unlocked(const char *path, struct ufs_stat *st)
{
    struct ufs_inode inode;
    uint32_t inode_num;

    if (st == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (resolve_path_ex(path, UFS_RESOLVE_NOFOLLOW_FINAL, &inode_num) != 0 ||
        read_inode(inode_num, &inode) != 0) {
        return -1;
    }
    return ufs_fill_stat_from_inode(&inode, st);
}

static int ufs_link_unlocked(const char *existing_path, const char *new_path)
{
    struct ufs_inode inode;
    struct ufs_inode parent_inode;
    char name[UFS_MAX_NAME + 1];
    uint32_t inode_num;
    uint32_t parent;
    uint32_t existing;

    if (existing_path == NULL || new_path == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (resolve_path_ex(existing_path, UFS_RESOLVE_NOFOLLOW_FINAL,
                        &inode_num) != 0 ||
        read_inode(inode_num, &inode) != 0) {
        return -1;
    }
    if (inode.type == UFS_TYPE_DIR) {
        errno = EPERM;
        return -1;
    }
    if (inode.type != UFS_TYPE_FILE && inode.type != UFS_TYPE_SYMLINK) {
        errno = EINVAL;
        return -1;
    }
    if (inode.link_count == UINT32_MAX) {
        errno = EMLINK;
        return -1;
    }
    if (resolve_parent(new_path, &parent, name) != 0 ||
        read_inode(parent, &parent_inode) != 0) {
        return -1;
    }
    if (ufs_check_access_inode(&parent_inode, W_OK | X_OK) != 0) {
        return -1;
    }
    if (directory_find(parent, name, &existing) == 0) {
        errno = EEXIST;
        return -1;
    }
    if (errno != ENOENT) {
        return -1;
    }

    if (directory_add(parent, name, inode_num, inode.type) != 0) {
        return -1;
    }
    ++inode.link_count;
    ufs_touch_ctime(&inode);
    if (write_inode(inode_num, &inode) != 0) {
        int saved_errno = errno;
        (void)directory_remove(parent, name);
        errno = saved_errno;
        return -1;
    }
    if (read_inode(parent, &parent_inode) != 0) {
        return -1;
    }
    ufs_touch_mtime_ctime(&parent_inode);
    if (write_inode(parent, &parent_inode) != 0) {
        return -1;
    }
    return 0;
}

static int ufs_symlink_unlocked(const char *target, const char *link_path)
{
    struct ufs_inode inode;
    struct ufs_inode parent_inode;
    uint8_t block[UFS_BLOCK_SIZE];
    char name[UFS_MAX_NAME + 1];
    uint32_t parent;
    uint32_t existing;
    uint32_t inode_num;
    uint32_t physical;
    uint32_t generation;
    size_t target_length;

    if (target == NULL || link_path == NULL) {
        errno = EINVAL;
        return -1;
    }
    target_length = strlen(target);
    if (target_length == 0) {
        errno = ENOENT;
        return -1;
    }
    if (target_length > UFS_MAX_PATH) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (resolve_parent(link_path, &parent, name) != 0 ||
        read_inode(parent, &parent_inode) != 0) {
        return -1;
    }
    if (ufs_check_access_inode(&parent_inode, W_OK | X_OK) != 0) {
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
    if (read_inode(inode_num, &inode) != 0) {
        int saved_errno = errno;
        (void)free_inode(inode_num);
        errno = saved_errno;
        return -1;
    }
    generation = inode.generation;
    ufs_init_inode(&inode, UFS_TYPE_SYMLINK);
    inode.generation = generation;
    if (ufs_initialize_metadata(&inode, UFS_TYPE_SYMLINK, 0777) != 0 ||
        allocate_data_block(&physical) != 0) {
        int saved_errno = errno;
        (void)free_inode(inode_num);
        errno = saved_errno;
        return -1;
    }

    inode.direct[0] = physical;
    inode.block_count = 1;
    inode.size = target_length;
    memset(block, 0, sizeof(block));
    memcpy(block, target, target_length);

    if (ufs_write_block(physical, block) != 0) {
        int saved_errno = errno;
        (void)free_data_block(physical);
        (void)free_inode(inode_num);
        errno = saved_errno;
        return -1;
    }
    if (write_inode(inode_num, &inode) != 0 ||
        directory_add(parent, name, inode_num, UFS_TYPE_SYMLINK) != 0) {
        int saved_errno = errno;
        (void)free_inode(inode_num);
        errno = saved_errno;
        return -1;
    }
    if (read_inode(parent, &parent_inode) != 0) {
        return -1;
    }
    ufs_touch_mtime_ctime(&parent_inode);
    if (write_inode(parent, &parent_inode) != 0) {
        return -1;
    }
    return 0;
}

static ssize_t ufs_readlink_unlocked(const char *path, char *buf, size_t size)
{
    struct ufs_inode inode;
    char target[UFS_MAX_PATH + 1];
    uint32_t inode_num;
    size_t copied;

    if (path == NULL || buf == NULL || size == 0) {
        errno = EINVAL;
        return -1;
    }
    if (resolve_path_ex(path, UFS_RESOLVE_NOFOLLOW_FINAL, &inode_num) != 0 ||
        read_inode(inode_num, &inode) != 0) {
        return -1;
    }
    if (inode.type != UFS_TYPE_SYMLINK) {
        errno = EINVAL;
        return -1;
    }
    if (read_symlink_target(&inode, target) != 0) {
        return -1;
    }
    copied = (size_t)inode.size < size ? (size_t)inode.size : size;
    memcpy(buf, target, copied);
    if (ufs_should_update_atime(&inode)) {
        ufs_touch_atime(&inode);
        if (write_inode(inode_num, &inode) != 0) {
            return -1;
        }
    }
    return (ssize_t)copied;
}

#define UFS_LOCKED_INT_WRAPPER(name, lock_function, parameters, arguments) \
    int name parameters                                                   \
    {                                                                     \
        int result;                                                       \
        int saved_errno;                                                  \
        if (lock_function() < 0) {                                        \
            return -1;                                                    \
        }                                                                 \
        result = name##_unlocked arguments;                               \
        saved_errno = errno;                                              \
        ufs_operation_unlock();                                           \
        errno = saved_errno;                                              \
        return result;                                                    \
    }

UFS_LOCKED_INT_WRAPPER(ufs_mkdir, ufs_operation_write_lock,
                       (const char *path), (path))
UFS_LOCKED_INT_WRAPPER(ufs_create, ufs_operation_write_lock,
                       (const char *path), (path))
UFS_LOCKED_INT_WRAPPER(ufs_rmdir, ufs_operation_write_lock,
                       (const char *path), (path))
UFS_LOCKED_INT_WRAPPER(ufs_unlink, ufs_operation_write_lock,
                       (const char *path), (path))
UFS_LOCKED_INT_WRAPPER(ufs_listdir, ufs_operation_read_lock,
                       (const char *path, struct ufs_dirent *entries,
                        size_t max_entries),
                       (path, entries, max_entries))
UFS_LOCKED_INT_WRAPPER(ufs_stat, ufs_operation_read_lock,
                       (const char *path, struct ufs_stat *st), (path, st))
UFS_LOCKED_INT_WRAPPER(ufs_lstat, ufs_operation_read_lock,
                       (const char *path, struct ufs_stat *st), (path, st))
UFS_LOCKED_INT_WRAPPER(ufs_link, ufs_operation_write_lock,
                       (const char *existing_path, const char *new_path),
                       (existing_path, new_path))
UFS_LOCKED_INT_WRAPPER(ufs_symlink, ufs_operation_write_lock,
                       (const char *target, const char *link_path),
                       (target, link_path))

ssize_t ufs_readlink(const char *path, char *buf, size_t size)
{
    ssize_t result;
    int saved_errno;

    if (ufs_operation_read_lock() < 0) {
        return -1;
    }
    result = ufs_readlink_unlocked(path, buf, size);
    saved_errno = errno;
    ufs_operation_unlock();
    errno = saved_errno;
    return result;
}
