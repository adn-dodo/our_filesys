#include "userfs.h"

#include <errno.h>

int ufs_format(const char *image_path, size_t image_size)
{
    (void)image_path;
    (void)image_size;
    errno = ENOSYS;
    return -1;
}

int ufs_mount(const char *image_path)
{
    (void)image_path;
    errno = ENOSYS;
    return -1;
}

int ufs_unmount(void)
{
    errno = ENOSYS;
    return -1;
}

int ufs_mkdir(const char *path)
{
    (void)path;
    errno = ENOSYS;
    return -1;
}

int ufs_rmdir(const char *path)
{
    (void)path;
    errno = ENOSYS;
    return -1;
}

int ufs_listdir(const char *path, struct ufs_dirent *entries, size_t max_entries)
{
    (void)path;
    (void)entries;
    (void)max_entries;
    errno = ENOSYS;
    return -1;
}

int ufs_create(const char *path)
{
    (void)path;
    errno = ENOSYS;
    return -1;
}

int ufs_unlink(const char *path)
{
    (void)path;
    errno = ENOSYS;
    return -1;
}

int ufs_open(const char *path, int flags)
{
    (void)path;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

int ufs_close(int fd)
{
    (void)fd;
    errno = ENOSYS;
    return -1;
}

ssize_t ufs_read(int fd, void *buf, size_t count)
{
    (void)fd;
    (void)buf;
    (void)count;
    errno = ENOSYS;
    return -1;
}

ssize_t ufs_write(int fd, const void *buf, size_t count)
{
    (void)fd;
    (void)buf;
    (void)count;
    errno = ENOSYS;
    return -1;
}

off_t ufs_seek(int fd, off_t offset, int whence)
{
    (void)fd;
    (void)offset;
    (void)whence;
    errno = ENOSYS;
    return -1;
}

int ufs_truncate(const char *path, size_t size)
{
    (void)path;
    (void)size;
    errno = ENOSYS;
    return -1;
}

int ufs_stat(const char *path, struct ufs_stat *st)
{
    (void)path;
    (void)st;
    errno = ENOSYS;
    return -1;
}