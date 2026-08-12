#include "file_io.h"
#include "ufs_internal.h"
#include "userfs.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define UFS_MAX_IO_COUNT ((size_t)(SIZE_MAX >> 1U))

/*
 * Integration contract with the other UserFS modules.
 * Every helper returns 0 on success or a negative errno value on failure.
 */
int ufs_is_mounted(void);
int resolve_path(const char *path, uint32_t *inode_number);
int read_inode(uint32_t inode_number, struct ufs_inode *inode);
int read_block(uint32_t block_number, void *buffer);
int write_block(uint32_t block_number, const void *buffer);
int get_inode_data_block(const struct ufs_inode *inode,
                         uint32_t logical_block,
                         uint32_t *physical_block);
int ufs_truncate_inode(uint32_t inode_number,
                       struct ufs_inode *inode,
                       uint64_t new_size);


static struct ufs_file_descriptor descriptor_table[UFS_MAX_OPEN_FILES];

static int fail_from_helper(int result);
static int require_mounted(void);
static int validate_open_flags(int flags);
static int allocate_descriptor(void);
static int validate_descriptor(int fd);
static int descriptor_can_read(int fd);
static int descriptor_can_write(int fd);
static int add_seek_offset(uint64_t base, off_t change, off_t *result);

void descriptor_table_reset(void)
{
    memset(descriptor_table, 0, sizeof(descriptor_table));
}

int inode_is_open(uint32_t inode_number)
{
    int fd;

    for (fd = 0; fd < UFS_MAX_OPEN_FILES; ++fd) {
        if (descriptor_table[fd].in_use &&
            descriptor_table[fd].inode_number == inode_number) {
            return 1;
        }
    }

    return 0;
}

static int fail_from_helper(int result)
{
    errno = result < 0 ? -result : EIO;
    return -1;
}

static int require_mounted(void)
{
    if (!ufs_is_mounted()) {
        errno = ENODEV;
        return -1;
    }

    return 0;
}

static int validate_open_flags(int flags)
{
    int access_mode = flags & UFS_O_RDWR;
    int unknown_bits = flags & ~(UFS_O_RDWR | UFS_O_APPEND);

    if (unknown_bits != 0 ||
        (access_mode != UFS_O_RDONLY &&
         access_mode != UFS_O_WRONLY &&
         access_mode != UFS_O_RDWR) ||
        ((flags & UFS_O_APPEND) && access_mode == UFS_O_RDONLY)) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

static int allocate_descriptor(void)
{
    int fd;

    for (fd = 0; fd < UFS_MAX_OPEN_FILES; ++fd) {
        if (!descriptor_table[fd].in_use) {
            return fd;
        }
    }

    errno = EMFILE;
    return -1;
}

static int validate_descriptor(int fd)
{
    if (fd < 0 || fd >= UFS_MAX_OPEN_FILES ||
        !descriptor_table[fd].in_use) {
        errno = EBADF;
        return -1;
    }

    return 0;
}

static int descriptor_can_read(int fd)
{
    return (descriptor_table[fd].flags & UFS_O_RDWR) != UFS_O_WRONLY;
}

static int descriptor_can_write(int fd)
{
    return (descriptor_table[fd].flags & UFS_O_RDWR) != UFS_O_RDONLY;
}

int ufs_open(const char *path, int flags)
{
    uint32_t inode_number;
    struct ufs_inode inode;
    int fd;
    int result;

    if (require_mounted() < 0) {
        return -1;
    }
    if (path == NULL || validate_open_flags(flags) < 0) {
        if (path == NULL) {
            errno = EINVAL;
        }
        return -1;
    }

    result = resolve_path(path, &inode_number);
    if (result < 0) {
        return fail_from_helper(result);
    }

    result = read_inode(inode_number, &inode);
    if (result < 0) {
        return fail_from_helper(result);
    }
    if (inode.type == UFS_TYPE_DIR) {
        errno = EISDIR;
        return -1;
    }
    if (inode.type != UFS_TYPE_FILE) {
        errno = EINVAL;
        return -1;
    }

    fd = allocate_descriptor();
    if (fd < 0) {
        return -1;
    }

    descriptor_table[fd].in_use = 1;
    descriptor_table[fd].inode_number = inode_number;
    descriptor_table[fd].offset = 0;
    descriptor_table[fd].flags = flags;
    return fd;
}

int ufs_close(int fd)
{
    if (require_mounted() < 0 || validate_descriptor(fd) < 0) {
        return -1;
    }

    memset(&descriptor_table[fd], 0, sizeof(descriptor_table[fd]));
    return 0;
}

ssize_t ufs_read(int fd, void *buf, size_t count)
{
    struct ufs_inode inode;
    uint8_t block[UFS_BLOCK_SIZE];
    size_t total = 0;
    uint64_t available;
    int result;

    if (require_mounted() < 0 || validate_descriptor(fd) < 0) {
        return -1;
    }
    if (!descriptor_can_read(fd)) {
        errno = EBADF;
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (buf == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (count > UFS_MAX_IO_COUNT) {
        errno = EINVAL;
        return -1;
    }

    result = read_inode(descriptor_table[fd].inode_number, &inode);
    if (result < 0) {
        return fail_from_helper(result);
    }

    if ((uint64_t)descriptor_table[fd].offset >= inode.size) {
        return 0;
    }

    available = inode.size - (uint64_t)descriptor_table[fd].offset;
    if ((uint64_t)count > available) {
        count = (size_t)available;
    }

    while (total < count) {
        uint64_t position = (uint64_t)descriptor_table[fd].offset;
        uint32_t logical_block = (uint32_t)(position / UFS_BLOCK_SIZE);
        size_t inside_block = (size_t)(position % UFS_BLOCK_SIZE);
        size_t chunk = UFS_BLOCK_SIZE - inside_block;
        uint32_t physical_block;

        if (chunk > count - total) {
            chunk = count - total;
        }

        result = get_inode_data_block(&inode, logical_block, &physical_block);
        if (result < 0) {
            if (total > 0) {
                return (ssize_t)total;
            }
            return fail_from_helper(result);
        }

        result = read_block(physical_block, block);
        if (result < 0) {
            if (total > 0) {
                return (ssize_t)total;
            }
            return fail_from_helper(result);
        }

        memcpy((uint8_t *)buf + total, block + inside_block, chunk);
        descriptor_table[fd].offset += (off_t)chunk;
        total += chunk;
    }

    return (ssize_t)total;
}

ssize_t ufs_write(int fd, const void *buf, size_t count)
{
    struct ufs_inode inode;
    uint8_t block[UFS_BLOCK_SIZE];
    uint64_t start;
    uint64_t end;
    size_t total = 0;
    int result;

    if (require_mounted() < 0 || validate_descriptor(fd) < 0) {
        return -1;
    }
    if (!descriptor_can_write(fd)) {
        errno = EBADF;
        return -1;
    }
    if (count == 0) {
        return 0;
    }
    if (buf == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (count > UFS_MAX_IO_COUNT) {
        errno = EINVAL;
        return -1;
    }

    result = read_inode(descriptor_table[fd].inode_number, &inode);
    if (result < 0) {
        return fail_from_helper(result);
    }

    if (descriptor_table[fd].flags & UFS_O_APPEND) {
        descriptor_table[fd].offset = (off_t)inode.size;
    }

    start = (uint64_t)descriptor_table[fd].offset;
    if ((uint64_t)count > UINT64_MAX - start ||
        start + (uint64_t)count > (uint64_t)INT64_MAX) {
        errno = EFBIG;
        return -1;
    }
    end = start + (uint64_t)count;

    if (end > inode.size) {
        result = ufs_truncate_inode(descriptor_table[fd].inode_number,
                                    &inode, end);
        if (result < 0) {
            return fail_from_helper(result);
        }
    }

    while (total < count) {
        uint64_t position = start + total;
        uint32_t logical_block = (uint32_t)(position / UFS_BLOCK_SIZE);
        size_t inside_block = (size_t)(position % UFS_BLOCK_SIZE);
        size_t chunk = UFS_BLOCK_SIZE - inside_block;
        uint32_t physical_block;

        if (chunk > count - total) {
            chunk = count - total;
        }

        result = get_inode_data_block(&inode, logical_block, &physical_block);
        if (result < 0) {
            if (total > 0) {
                descriptor_table[fd].offset = (off_t)(start + total);
                return (ssize_t)total;
            }
            return fail_from_helper(result);
        }

        if (inside_block == 0 && chunk == UFS_BLOCK_SIZE) {
            memcpy(block, (const uint8_t *)buf + total, UFS_BLOCK_SIZE);
        } else {
            result = read_block(physical_block, block);
            if (result < 0) {
                if (total > 0) {
                    descriptor_table[fd].offset = (off_t)(start + total);
                    return (ssize_t)total;
                }
                return fail_from_helper(result);
            }
            memcpy(block + inside_block, (const uint8_t *)buf + total, chunk);
        }

        result = write_block(physical_block, block);
        if (result < 0) {
            if (total > 0) {
                descriptor_table[fd].offset = (off_t)(start + total);
                return (ssize_t)total;
            }
            return fail_from_helper(result);
        }

        total += chunk;
    }

    descriptor_table[fd].offset = (off_t)(start + total);
    return (ssize_t)total;
}

static int add_seek_offset(uint64_t base, off_t change, off_t *result)
{
    uint64_t answer;

    if (change >= 0) {
        uint64_t positive = (uint64_t)change;
        if (base > (uint64_t)INT64_MAX - positive) {
            errno = EINVAL;
            return -1;
        }
        answer = base + positive;
    } else {
        uint64_t magnitude = (uint64_t)(-(change + 1)) + 1U;
        if (magnitude > base) {
            errno = EINVAL;
            return -1;
        }
        answer = base - magnitude;
    }

    *result = (off_t)answer;
    return 0;
}

off_t ufs_seek(int fd, off_t offset, int whence)
{
    struct ufs_inode inode;
    uint64_t base;
    off_t new_offset;
    int result;

    if (require_mounted() < 0 || validate_descriptor(fd) < 0) {
        return (off_t)-1;
    }

    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = (uint64_t)descriptor_table[fd].offset;
        break;
    case SEEK_END:
        result = read_inode(descriptor_table[fd].inode_number, &inode);
        if (result < 0) {
            fail_from_helper(result);
            return (off_t)-1;
        }
        base = inode.size;
        break;
    default:
        errno = EINVAL;
        return (off_t)-1;
    }

    if (add_seek_offset(base, offset, &new_offset) < 0) {
        return (off_t)-1;
    }

    descriptor_table[fd].offset = new_offset;
    return new_offset;
}
