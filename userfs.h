#ifndef USERFS_H
#define USERFS_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UFS_BLOCK_SIZE 512
#define UFS_MAX_NAME 31
#define UFS_MAX_PATH 255
#define UFS_MAX_OPEN_FILES 32

#define UFS_TYPE_FILE 1
#define UFS_TYPE_DIR 2

#define UFS_O_RDONLY 0x1
#define UFS_O_WRONLY 0x2
#define UFS_O_RDWR 0x3
#define UFS_O_APPEND 0x4

struct ufs_stat {
    int type;
    size_t size;
};

struct ufs_dirent {
    char name[UFS_MAX_NAME + 1];
    int type;
    size_t size;
};

int ufs_format(const char *image_path, size_t image_size);
int ufs_mount(const char *image_path);
int ufs_unmount(void);

int ufs_mkdir(const char *path);
int ufs_rmdir(const char *path);
int ufs_listdir(const char *path, struct ufs_dirent *entries, size_t max_entries);

int ufs_create(const char *path);
int ufs_unlink(const char *path);
int ufs_open(const char *path, int flags);
int ufs_close(int fd);
ssize_t ufs_read(int fd, void *buf, size_t count);
ssize_t ufs_write(int fd, const void *buf, size_t count);
off_t ufs_seek(int fd, off_t offset, int whence);
int ufs_truncate(const char *path, size_t size);
int ufs_stat(const char *path, struct ufs_stat *st);

#ifdef __cplusplus
}
#endif

#endif