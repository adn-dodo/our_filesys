#include "userfs.h"
#include "ufs_internal.h"
#include "file_io.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>


/* Temporary descriptor information—never stored in disk image. */
struct ufs_file_descriptor {
    int in_use;
    uint32_t inode_number;
    off_t offset;
    int flags;
};

/* Maximum 32 simultaneously opened files. */
static struct ufs_file_descriptor
    descriptor_table[UFS_MAX_OPEN_FILES];

/* ---------- Private descriptor helpers ---------- */

static int allocate_descriptor(void);
static int validate_descriptor(int fd);
static int validate_open_flags(int flags);

/* ---------- Helpers used by other members ---------- */

void descriptor_table_reset(void);
int inode_is_open(uint32_t inode_number);

/* ---------- Public functions owned by Member 4 ---------- */

int ufs_open(const char *path, int flags)
{
    /* TODO */
    errno = ENOSYS;
    return -1;
}

int ufs_close(int fd)
{
    /* TODO */
    errno = ENOSYS;
    return -1;
}

ssize_t ufs_read(int fd, void *buf, size_t count)
{
    /* TODO */
    errno = ENOSYS;
    return -1;
}

ssize_t ufs_write(int fd, const void *buf, size_t count)
{
    /* TODO */
    errno = ENOSYS;
    return -1;
}

off_t ufs_seek(int fd, off_t offset, int whence)
{
    /* TODO */
    errno = ENOSYS;
    return -1;
}

/* ---------- Descriptor helper implementations ---------- */

void descriptor_table_reset(void)
{
    memset(descriptor_table, 0, sizeof(descriptor_table));
}

static int allocate_descriptor(void)
{
    for (int fd = 0; fd < UFS_MAX_OPEN_FILES; fd++) {
        if (!descriptor_table[fd].in_use) {
            return fd;
        }
    }

    errno = EMFILE;
    return -1;
}

static int validate_descriptor(int fd)
{
    if (fd < 0 ||
        fd >= UFS_MAX_OPEN_FILES ||
        !descriptor_table[fd].in_use) {
        errno = EBADF;
        return -1;
    }

    return 0;
}

static int validate_open_flags(int flags)
{
    int access_mode = flags & UFS_O_RDWR;
    int unknown = flags & ~(UFS_O_RDWR | UFS_O_APPEND);

    if (unknown != 0) {
        errno = EINVAL;
        return -1;
    }

    if (access_mode != UFS_O_RDONLY &&
        access_mode != UFS_O_WRONLY &&
        access_mode != UFS_O_RDWR) {
        errno = EINVAL;
        return -1;
    }

    if ((flags & UFS_O_APPEND) &&
        access_mode == UFS_O_RDONLY) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

int inode_is_open(uint32_t inode_number)
{
    for (int fd = 0; fd < UFS_MAX_OPEN_FILES; fd++) {
        if (descriptor_table[fd].in_use &&
            descriptor_table[fd].inode_number == inode_number) {
            return 1;
        }
    }

    return 0;
}
