#ifndef USERFS_H
#define USERFS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UFS_BLOCK_SIZE 512
#define UFS_MAX_NAME 31
#define UFS_MAX_PATH 255
#define UFS_MAX_OPEN_FILES 32

#define UFS_TYPE_FILE 1
#define UFS_TYPE_DIR 2
#define UFS_TYPE_SYMLINK 3

#define UFS_O_RDONLY 0x1
#define UFS_O_WRONLY 0x2
#define UFS_O_RDWR 0x3
#define UFS_O_APPEND 0x4

/* Copy-backed UserFS memory-mapping flags. */
#define UFS_PROT_READ   0x01
#define UFS_PROT_WRITE  0x02

#define UFS_MAP_SHARED  0x01
#define UFS_MAP_PRIVATE 0x02
#define UFS_MAP_FAILED  ((void *)-1)

struct ufs_stat {
    int type;
    uint64_t size;
    uint32_t blocks;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    uint32_t link_count;
    struct timespec atime;
    struct timespec mtime;
    struct timespec ctime;
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
int ufs_link(const char *existing_path, const char *new_path);
int ufs_symlink(const char *target, const char *link_path);
ssize_t ufs_readlink(const char *path, char *buf, size_t size);
int ufs_lstat(const char *path, struct ufs_stat *st);
int ufs_open(const char *path, int flags);
int ufs_close(int fd);
ssize_t ufs_read(int fd, void *buf, size_t count);
ssize_t ufs_write(int fd, const void *buf, size_t count);
off_t ufs_seek(int fd, off_t offset, int whence);
int ufs_truncate(const char *path, size_t size);
int ufs_stat(const char *path, struct ufs_stat *st);

void *ufs_mmap(int fd, size_t length, int prot, int flags, off_t offset);
int ufs_msync(void *address, size_t length);
int ufs_munmap(void *address, size_t length);

mode_t ufs_set_umask(mode_t new_mask);
int ufs_chmod(const char *path, mode_t mode);
int ufs_chown(const char *path, uid_t uid, gid_t gid);
int ufs_access(const char *path, int mode);
int ufs_utimens(const char *path, const struct timespec times[2]);

#ifdef __cplusplus
}
#endif

#endif
