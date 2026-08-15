#define _POSIX_C_SOURCE 200809L

#include "userfs.h"
#include "ufs_internal.h"
#include "file_io.h"
#include "journal.h"
#include "mmap_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct ufs_context g_ufs = {
    .mounted = 0,
    .fd = -1
};

static int read_exact(int fd, void *buffer, size_t count, off_t offset)
{
    size_t total = 0;
    uint8_t *bytes = buffer;

    while (total < count) {
        ssize_t amount = pread(fd, bytes + total, count - total,
                               offset + (off_t)total);
        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (amount == 0) {
            errno = EIO;
            return -1;
        }
        total += (size_t)amount;
    }
    return 0;
}

static int write_exact(int fd, const void *buffer, size_t count, off_t offset)
{
    size_t total = 0;
    const uint8_t *bytes = buffer;

    while (total < count) {
        ssize_t amount = pwrite(fd, bytes + total, count - total,
                                offset + (off_t)total);
        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (amount == 0) {
            errno = EIO;
            return -1;
        }
        total += (size_t)amount;
    }
    return 0;
}

int ufs_is_mounted(void)
{
    return g_ufs.mounted;
}

int ufs_read_block(uint32_t block_number, void *buffer)
{
    if (g_ufs.fd < 0) {
        errno = ENODEV;
        return -1;
    }
    if (buffer == NULL || block_number >= UFS_TOTAL_BLOCKS) {
        errno = EINVAL;
        return -1;
    }
    return read_exact(g_ufs.fd, buffer, UFS_BLOCK_SIZE,
                      (off_t)block_number * UFS_BLOCK_SIZE);
}

int ufs_write_block(uint32_t block_number, const void *buffer)
{
    if (g_ufs.fd < 0) {
        errno = ENODEV;
        return -1;
    }
    if (buffer == NULL || block_number >= UFS_TOTAL_BLOCKS) {
        errno = EINVAL;
        return -1;
    }
    return write_exact(g_ufs.fd, buffer, UFS_BLOCK_SIZE,
                       (off_t)block_number * UFS_BLOCK_SIZE);
}

int ufs_flush_superblock(void)
{
    if (!g_ufs.mounted || g_ufs.fd < 0) {
        errno = ENODEV;
        return -1;
    }
    return ufs_write_block(UFS_SUPERBLOCK_BLK, &g_ufs.sb);
}

int ufs_sync_image(void)
{
    if (g_ufs.fd < 0) {
        errno = ENODEV;
        return -1;
    }
    return fsync(g_ufs.fd);
}

void ufs_init_inode(struct ufs_inode *inode, uint32_t type)
{
    uint32_t index;

    if (inode == NULL) {
        return;
    }
    memset(inode, 0, sizeof(*inode));
    inode->type = type;
    for (index = 0; index < UFS_DIRECT_BLOCKS; ++index) {
        inode->direct[index] = UFS_INVALID_BLOCK;
    }
    inode->single_indirect = UFS_INVALID_BLOCK;
    inode->double_indirect = UFS_INVALID_BLOCK;
}

static int validate_superblock(const struct ufs_superblock *sb)
{
    if (sb == NULL || sb->magic != UFS_MAGIC ||
        sb->version != UFS_VERSION || sb->block_size != UFS_BLOCK_SIZE ||
        sb->total_blocks != UFS_TOTAL_BLOCKS ||
        sb->inode_bitmap_start != UFS_INODE_BITMAP_BLK ||
        sb->inode_bitmap_blocks != UFS_INODE_BITMAP_BLOCKS ||
        sb->block_bitmap_start != UFS_BLOCK_BITMAP_BLK ||
        sb->block_bitmap_blocks != UFS_BLOCK_BITMAP_BLOCKS ||
        sb->inode_table_start != UFS_INODE_TABLE_START_BLK ||
        sb->inode_table_blocks != UFS_INODE_TABLE_BLOCKS ||
        sb->data_region_start != UFS_DATA_REGION_START_BLK ||
        sb->data_region_blocks != UFS_DATA_REGION_BLOCKS ||
        sb->journal_start != UFS_JOURNAL_START_BLK ||
        sb->journal_blocks != UFS_JOURNAL_BLOCKS ||
        sb->total_inodes != UFS_MAX_INODES ||
        sb->root_inode != UFS_ROOT_INODE ||
        sb->free_inodes > UFS_MAX_INODES ||
        sb->free_blocks > UFS_DATA_REGION_BLOCKS) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int close_format_fd(int fd, int result)
{
    int saved_errno = errno;

    if (close(fd) < 0 && result == 0) {
        saved_errno = errno;
        result = -1;
    }
    g_ufs.fd = -1;
    errno = saved_errno;
    return result;
}

int ufs_format(const char *image_path, size_t image_size)
{
    uint8_t block[UFS_BLOCK_SIZE];
    uint8_t inode_bitmap[UFS_BLOCK_SIZE];
    uint8_t block_bitmap[UFS_BLOCK_BITMAP_BLOCKS * UFS_BLOCK_SIZE];
    struct ufs_superblock sb;
    struct ufs_inode root;
    uint32_t block_number;
    uint32_t bitmap_block;
    struct timespec now;
    int fd;

    if (image_path == NULL || image_path[0] == '\0' ||
        image_size != UFS_IMAGE_SIZE) {
        errno = EINVAL;
        return -1;
    }
    if (g_ufs.mounted) {
        errno = EBUSY;
        return -1;
    }

    fd = open(image_path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return -1;
    }
    g_ufs.fd = fd;

    if (ftruncate(fd, (off_t)UFS_IMAGE_SIZE) < 0) {
        return close_format_fd(fd, -1);
    }

    memset(block, 0, sizeof(block));
    for (block_number = 0; block_number < UFS_TOTAL_BLOCKS; ++block_number) {
        if (ufs_write_block(block_number, block) < 0) {
            return close_format_fd(fd, -1);
        }
    }

    memset(&sb, 0, sizeof(sb));
    sb.magic = UFS_MAGIC;
    sb.version = UFS_VERSION;
    sb.block_size = UFS_BLOCK_SIZE;
    sb.total_blocks = UFS_TOTAL_BLOCKS;
    sb.inode_bitmap_start = UFS_INODE_BITMAP_BLK;
    sb.inode_bitmap_blocks = UFS_INODE_BITMAP_BLOCKS;
    sb.block_bitmap_start = UFS_BLOCK_BITMAP_BLK;
    sb.block_bitmap_blocks = UFS_BLOCK_BITMAP_BLOCKS;
    sb.inode_table_start = UFS_INODE_TABLE_START_BLK;
    sb.inode_table_blocks = UFS_INODE_TABLE_BLOCKS;
    sb.data_region_start = UFS_DATA_REGION_START_BLK;
    sb.data_region_blocks = UFS_DATA_REGION_BLOCKS;
    sb.total_inodes = UFS_MAX_INODES;
    sb.root_inode = UFS_ROOT_INODE;
    sb.free_inodes = UFS_MAX_INODES - 1U;
    sb.free_blocks = UFS_DATA_REGION_BLOCKS;
    sb.journal_start = UFS_JOURNAL_START_BLK;
    sb.journal_blocks = UFS_JOURNAL_BLOCKS;
    sb.journal_sequence = 1U;
    sb.state = UFS_STATE_CLEAN;
    sb.features = UFS_FEATURE_JOURNAL | UFS_FEATURE_METADATA |
                  UFS_FEATURE_LINKS;
    if (ufs_write_block(UFS_SUPERBLOCK_BLK, &sb) < 0) {
        return close_format_fd(fd, -1);
    }

    memset(inode_bitmap, 0, sizeof(inode_bitmap));
    inode_bitmap[UFS_ROOT_INODE / 8U] |=
        (uint8_t)(1U << (UFS_ROOT_INODE % 8U));
    if (ufs_write_block(UFS_INODE_BITMAP_BLK, inode_bitmap) < 0) {
        return close_format_fd(fd, -1);
    }

    memset(block_bitmap, 0, sizeof(block_bitmap));
    for (block_number = 0; block_number < UFS_DATA_REGION_START_BLK;
         ++block_number) {
        block_bitmap[block_number / 8U] |=
            (uint8_t)(1U << (block_number % 8U));
    }
    for (bitmap_block = 0; bitmap_block < UFS_BLOCK_BITMAP_BLOCKS;
         ++bitmap_block) {
        if (ufs_write_block(UFS_BLOCK_BITMAP_START_BLK + bitmap_block,
                            block_bitmap +
                                (size_t)bitmap_block * UFS_BLOCK_SIZE) < 0) {
            return close_format_fd(fd, -1);
        }
    }

    ufs_init_inode(&root, UFS_TYPE_DIR);
    root.mode = 0755U;
    root.uid = (uint32_t)geteuid();
    root.gid = (uint32_t)getegid();
    root.link_count = 1U;
    root.generation = 1U;
    if (clock_gettime(CLOCK_REALTIME, &now) < 0) {
        return close_format_fd(fd, -1);
    }
    root.atime.sec = (int64_t)now.tv_sec;
    root.atime.nsec = (int32_t)now.tv_nsec;
    root.mtime = root.atime;
    root.ctime = root.atime;
    memset(block, 0, sizeof(block));
    memcpy(block, &root, sizeof(root));
    if (ufs_write_block(UFS_INODE_TABLE_START_BLK, block) < 0) {
        return close_format_fd(fd, -1);
    }

    if (fsync(fd) < 0) {
        return close_format_fd(fd, -1);
    }
    return close_format_fd(fd, 0);
}

int ufs_mount(const char *image_path)
{
    struct ufs_superblock sb;
    struct stat image_stat;
    int fd;

    if (image_path == NULL || image_path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (g_ufs.mounted) {
        errno = EBUSY;
        return -1;
    }

    fd = open(image_path, O_RDWR);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &image_stat) < 0) {
        int saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        return -1;
    }
    if (image_stat.st_size != (off_t)UFS_IMAGE_SIZE) {
        (void)close(fd);
        errno = EINVAL;
        return -1;
    }

    g_ufs.fd = fd;
    if (ufs_read_block(UFS_SUPERBLOCK_BLK, &sb) < 0 ||
        validate_superblock(&sb) < 0) {
        int saved_errno = errno;
        (void)close(fd);
        g_ufs.fd = -1;
        errno = saved_errno;
        return -1;
    }

    g_ufs.sb = sb;
    if (ufs_journal_recover() < 0 ||
        ufs_read_block(UFS_SUPERBLOCK_BLK, &sb) < 0 ||
        validate_superblock(&sb) < 0) {
        int saved_errno = errno;
        (void)close(fd);
        g_ufs.fd = -1;
        memset(&g_ufs.sb, 0, sizeof(g_ufs.sb));
        errno = saved_errno;
        return -1;
    }

    g_ufs.sb = sb;
    g_ufs.mounted = 1;
    g_ufs.sb.state = UFS_STATE_DIRTY;
    if (ufs_flush_superblock() < 0 || ufs_sync_image() < 0) {
        int saved_errno = errno;
        (void)close(fd);
        g_ufs.fd = -1;
        g_ufs.mounted = 0;
        memset(&g_ufs.sb, 0, sizeof(g_ufs.sb));
        errno = saved_errno;
        return -1;
    }
    descriptor_table_reset();
    return 0;
}

int ufs_unmount(void)
{
    int result = 0;
    int saved_errno = 0;

    if (!g_ufs.mounted) {
        errno = EINVAL;
        return -1;
    }
    if (ufs_mmap_has_active()) {
        errno = EBUSY;
        return -1;
    }
    g_ufs.sb.state = UFS_STATE_CLEAN;
    if (ufs_flush_superblock() < 0 || fsync(g_ufs.fd) < 0) {
        result = -1;
        saved_errno = errno;
    }
    if (close(g_ufs.fd) < 0 && result == 0) {
        result = -1;
        saved_errno = errno;
    }

    g_ufs.fd = -1;
    g_ufs.mounted = 0;
    memset(&g_ufs.sb, 0, sizeof(g_ufs.sb));
    descriptor_table_reset();
    if (result < 0) {
        errno = saved_errno;
    }
    return result;
}
