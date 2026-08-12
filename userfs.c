#define _POSIX_C_SOURCE 200809L

#include "userfs.h"
#include "ufs_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* =========================================================
 * Mounted filesystem state
 * ========================================================= */

struct ufs_mount_state {
    int mounted;
    int fd;
    struct ufs_superblock sb;
    struct ufs_file_descriptor descriptors[UFS_MAX_OPEN_FILES];
};

static struct ufs_mount_state g_fs = {
    .mounted = 0,
    .fd = -1
};

/* =========================================================
 * Exact Linux image I/O helpers
 * ========================================================= */

/*
 * Read exactly 'count' bytes starting at 'offset'.
 *
 * Returns:
 *   0  on success
 *  -1  on error
 */
static int read_exact(int fd,
                      void *buffer,
                      size_t count,
                      off_t offset)
{
    size_t total = 0;
    char *ptr = buffer;

    while (total < count) {
        ssize_t n = pread(fd,
                          ptr + total,
                          count - total,
                          offset + (off_t)total);

        if (n < 0) {
            return -1;
        }

        if (n == 0) {
            errno = EIO;
            return -1;
        }

        total += (size_t)n;
    }

    return 0;
}

/*
 * Write exactly 'count' bytes starting at 'offset'.
 *
 * Returns:
 *   0  on success
 *  -1  on error
 */
static int write_exact(int fd,
                       const void *buffer,
                       size_t count,
                       off_t offset)
{
    size_t total = 0;
    const char *ptr = buffer;

    while (total < count) {
        ssize_t n = pwrite(fd,
                           ptr + total,
                           count - total,
                           offset + (off_t)total);

        if (n < 0) {
            return -1;
        }

        if (n == 0) {
            errno = EIO;
            return -1;
        }

        total += (size_t)n;
    }

    return 0;
}

/*
 * Read one complete 512-byte filesystem block.
 */
static int read_block(uint32_t block_number, void *buffer)
{
    if (g_fs.fd < 0) {
        errno = EBADF;
        return -1;
    }

    if (buffer == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (block_number >= UFS_TOTAL_BLOCKS) {
        errno = EINVAL;
        return -1;
    }

    return read_exact(
        g_fs.fd,
        buffer,
        UFS_BLOCK_SIZE,
        (off_t)block_number * UFS_BLOCK_SIZE
    );
}

/*
 * Write one complete 512-byte filesystem block.
 */
static int write_block(uint32_t block_number, const void *buffer)
{
    if (g_fs.fd < 0) {
        errno = EBADF;
        return -1;
    }

    if (buffer == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (block_number >= UFS_TOTAL_BLOCKS) {
        errno = EINVAL;
        return -1;
    }

    return write_exact(
        g_fs.fd,
        buffer,
        UFS_BLOCK_SIZE,
        (off_t)block_number * UFS_BLOCK_SIZE
    );
}

/* =========================================================
 * Descriptor table
 * ========================================================= */

static void reset_descriptors(void)
{
    size_t i;

    for (i = 0; i < UFS_MAX_OPEN_FILES; ++i) {
        g_fs.descriptors[i].in_use = 0;
        g_fs.descriptors[i].inode_number = 0;
        g_fs.descriptors[i].offset = 0;
        g_fs.descriptors[i].flags = 0;
    }
}

/* =========================================================
 * Superblock validation
 * ========================================================= */

static int validate_superblock(const struct ufs_superblock *sb)
{
    if (sb == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (sb->magic != UFS_MAGIC) {
        errno = EINVAL;
        return -1;
    }

    if (sb->version != UFS_VERSION) {
        errno = EINVAL;
        return -1;
    }

    if (sb->block_size != UFS_BLOCK_SIZE) {
        errno = EINVAL;
        return -1;
    }

    if (sb->total_blocks != UFS_TOTAL_BLOCKS) {
        errno = EINVAL;
        return -1;
    }

    if (sb->inode_bitmap_start != UFS_INODE_BITMAP_BLK ||
        sb->inode_bitmap_blocks != 1U) {
        errno = EINVAL;
        return -1;
    }

    if (sb->block_bitmap_start != UFS_BLOCK_BITMAP_BLK ||
        sb->block_bitmap_blocks != 1U) {
        errno = EINVAL;
        return -1;
    }

    if (sb->inode_table_start != UFS_INODE_TABLE_START_BLK ||
        sb->inode_table_blocks != UFS_INODE_TABLE_BLOCKS) {
        errno = EINVAL;
        return -1;
    }

    if (sb->data_region_start != UFS_DATA_REGION_START_BLK ||
        sb->data_region_blocks != UFS_DATA_REGION_BLOCKS) {
        errno = EINVAL;
        return -1;
    }

    if (sb->total_inodes != UFS_MAX_INODES) {
        errno = EINVAL;
        return -1;
    }

    if (sb->root_inode != UFS_ROOT_INODE) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

/* =========================================================
 * ufs_format
 * ========================================================= */

int ufs_format(const char *image_path, size_t image_size)
{
    int fd;
    struct ufs_superblock sb;
    struct ufs_inode root_inode;

    unsigned char zero_block[UFS_BLOCK_SIZE];
    unsigned char inode_bitmap[UFS_BLOCK_SIZE];
    unsigned char block_bitmap[UFS_BLOCK_SIZE];
    unsigned char inode_table_block[UFS_BLOCK_SIZE];

    /*
     * Validate path.
     */
    if (image_path == NULL) {
        errno = EINVAL;
        return -1;
    }

    /*
     * This filesystem has a fixed size of 1 MiB.
     */
    if (image_size != UFS_IMAGE_SIZE) {
        errno = EINVAL;
        return -1;
    }

    /*
     * Do not format a mounted filesystem.
     */
    if (g_fs.mounted) {
        errno = EBUSY;
        return -1;
    }

    /*
     * Create or overwrite the disk image.
     */
    fd = open(image_path,
              O_RDWR | O_CREAT | O_TRUNC,
              0666);

    if (fd < 0) {
        return -1;
    }

    /*
     * Make this descriptor temporarily available
     * to the low-level block I/O helpers.
     */
    g_fs.fd = fd;

    /*
     * Set image size to exactly 1 MiB.
     */
    if (ftruncate(fd, (off_t)UFS_IMAGE_SIZE) < 0) {
        int saved_errno = errno;

        close(fd);
        g_fs.fd = -1;

        errno = saved_errno;
        return -1;
    }

    /*
     * Prepare a zero-filled block.
     */
    memset(zero_block, 0, sizeof(zero_block));

    /*
     * Zero the complete image.
     *
     * This guarantees that all metadata starts from
     * a known state.
     */
    for (uint32_t block = 0;
         block < UFS_TOTAL_BLOCKS;
         ++block) {

        if (write_block(block, zero_block) < 0) {
            int saved_errno = errno;

            close(fd);
            g_fs.fd = -1;

            errno = saved_errno;
            return -1;
        }
    }

    /*
     * =====================================================
     * Create the superblock
     * =====================================================
     */

    memset(&sb, 0, sizeof(sb));

    sb.magic = UFS_MAGIC;
    sb.version = UFS_VERSION;
    sb.block_size = UFS_BLOCK_SIZE;
    sb.total_blocks = UFS_TOTAL_BLOCKS;

    sb.inode_bitmap_start = UFS_INODE_BITMAP_BLK;
    sb.inode_bitmap_blocks = 1U;

    sb.block_bitmap_start = UFS_BLOCK_BITMAP_BLK;
    sb.block_bitmap_blocks = 1U;

    sb.inode_table_start = UFS_INODE_TABLE_START_BLK;
    sb.inode_table_blocks = UFS_INODE_TABLE_BLOCKS;

    sb.data_region_start = UFS_DATA_REGION_START_BLK;
    sb.data_region_blocks = UFS_DATA_REGION_BLOCKS;

    sb.total_inodes = UFS_MAX_INODES;
    sb.root_inode = UFS_ROOT_INODE;

    /*
     * Inode 0 belongs to the root directory.
     *
     * 256 total inodes - 1 used inode = 255 free.
     */
    sb.free_inodes = UFS_MAX_INODES - 1U;

    /*
     * Blocks 35..2047 are data blocks.
     *
     * At format time the root directory has no allocated
     * data block yet, so all 2013 data blocks are free.
     */
    sb.free_blocks = UFS_DATA_REGION_BLOCKS;

    /*
     * Write superblock to block 0.
     */
    if (write_block(UFS_SUPERBLOCK_BLK, &sb) < 0) {
        int saved_errno = errno;

        close(fd);
        g_fs.fd = -1;

        errno = saved_errno;
        return -1;
    }

    /*
     * =====================================================
     * Inode bitmap
     * =====================================================
     *
     * inode 0 = occupied
     * inode 1..255 = free
     */

    memset(inode_bitmap, 0, sizeof(inode_bitmap));

    inode_bitmap[0] |= 1U;

    if (write_block(UFS_INODE_BITMAP_BLK,
                    inode_bitmap) < 0) {

        int saved_errno = errno;

        close(fd);
        g_fs.fd = -1;

        errno = saved_errno;
        return -1;
    }

    /*
     * =====================================================
     * Block bitmap
     * =====================================================
     *
     * Blocks 0..34 are metadata blocks.
     * They must always be marked occupied.
     *
     * Blocks 35..2047 are data blocks and start free.
     */

    memset(block_bitmap, 0, sizeof(block_bitmap));

    for (uint32_t block = 0;
         block < UFS_DATA_REGION_START_BLK;
         ++block) {

        block_bitmap[block / 8U] |=
            (unsigned char)(1U << (block % 8U));
    }

    if (write_block(UFS_BLOCK_BITMAP_BLK,
                    block_bitmap) < 0) {

        int saved_errno = errno;

        close(fd);
        g_fs.fd = -1;

        errno = saved_errno;
        return -1;
    }

    /*
     * =====================================================
     * Root inode
     * =====================================================
     */

    memset(&root_inode, 0, sizeof(root_inode));

    root_inode.type = UFS_TYPE_DIR;
    root_inode.flags = 0;
    root_inode.size = 0;
    root_inode.block_count = 0;

    /*
     * All unused block pointers must contain
     * UFS_INVALID_BLOCK.
     */
    for (size_t i = 0;
         i < UFS_DIRECT_BLOCKS;
         ++i) {

        root_inode.direct[i] = UFS_INVALID_BLOCK;
    }

    root_inode.single_indirect = UFS_INVALID_BLOCK;
    root_inode.double_indirect = UFS_INVALID_BLOCK;
    root_inode.reserved = 0;

    /*
     * =====================================================
     * Inode table
     * =====================================================
     *
     * Inode table starts at block 3.
     *
     * Each inode is 64 bytes.
     * Each block is 512 bytes.
     *
     * Therefore:
     *
     * 512 / 64 = 8 inodes per block.
     *
     * Inode 0 is the first inode in block 3.
     */

    memset(inode_table_block, 0, sizeof(inode_table_block));

    memcpy(inode_table_block,
           &root_inode,
           sizeof(root_inode));

    if (write_block(UFS_INODE_TABLE_START_BLK,
                    inode_table_block) < 0) {

        int saved_errno = errno;

        close(fd);
        g_fs.fd = -1;

        errno = saved_errno;
        return -1;
    }

    /*
     * Flush all format changes.
     */
    if (fsync(fd) < 0) {
        int saved_errno = errno;

        close(fd);
        g_fs.fd = -1;

        errno = saved_errno;
        return -1;
    }

    /*
     * Close the image.
     */
    if (close(fd) < 0) {
        int saved_errno = errno;

        g_fs.fd = -1;

        errno = saved_errno;
        return -1;
    }

    g_fs.fd = -1;

    return 0;
}

/* =========================================================
 * ufs_mount
 * ========================================================= */

int ufs_mount(const char *image_path)
{
    int fd;
    struct ufs_superblock sb;

    /*
     * Validate path.
     */
    if (image_path == NULL) {
        errno = EINVAL;
        return -1;
    }

    /*
     * Only one filesystem can be mounted at a time.
     */
    if (g_fs.mounted) {
        errno = EBUSY;
        return -1;
    }

    /*
     * Open existing disk image.
     */
    fd = open(image_path, O_RDWR);

    if (fd < 0) {
        return -1;
    }

    /*
     * Temporarily store the descriptor so read_block()
     * can access the image.
     */
    g_fs.fd = fd;

    /*
     * Read the superblock from block 0.
     */
    if (read_block(UFS_SUPERBLOCK_BLK, &sb) < 0) {
        int saved_errno = errno;

        close(fd);
        g_fs.fd = -1;

        errno = saved_errno;
        return -1;
    }

    /*
     * Validate filesystem identity and layout.
     */
    if (validate_superblock(&sb) < 0) {
        int saved_errno = errno;

        close(fd);
        g_fs.fd = -1;

        errno = saved_errno;
        return -1;
    }

    /*
     * The image is valid.
     *
     * Save the superblock in memory and mark the
     * filesystem as mounted.
     */
    g_fs.sb = sb;
    g_fs.mounted = 1;

    /*
     * Every mount starts with an empty descriptor table.
     */
    reset_descriptors();

    return 0;
}

/* =========================================================
 * ufs_unmount
 * ========================================================= */

int ufs_unmount(void)
{
    int saved_errno;

    /*
     * Nothing is mounted.
     */
    if (!g_fs.mounted) {
        errno = EINVAL;
        return -1;
    }

    /*
     * Flush filesystem changes.
     */
    if (fsync(g_fs.fd) < 0) {
        saved_errno = errno;

        close(g_fs.fd);

        g_fs.fd = -1;
        g_fs.mounted = 0;

        memset(&g_fs.sb, 0, sizeof(g_fs.sb));
        reset_descriptors();

        errno = saved_errno;
        return -1;
    }

    /*
     * Close Linux image descriptor.
     */
    if (close(g_fs.fd) < 0) {
        saved_errno = errno;

        g_fs.fd = -1;
        g_fs.mounted = 0;

        memset(&g_fs.sb, 0, sizeof(g_fs.sb));
        reset_descriptors();

        errno = saved_errno;
        return -1;
    }

    /*
     * Clear mounted state.
     */
    g_fs.fd = -1;
    g_fs.mounted = 0;

    memset(&g_fs.sb, 0, sizeof(g_fs.sb));

    reset_descriptors();

    return 0;
}

/* =========================================================
 * Functions owned by other members
 * ========================================================= */

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

int ufs_listdir(const char *path,
                struct ufs_dirent *entries,
                size_t max_entries)
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

ssize_t ufs_write(int fd,
                  const void *buf,
                  size_t count)
{
    (void)fd;
    (void)buf;
    (void)count;

    errno = ENOSYS;
    return -1;
}

off_t ufs_seek(int fd,
               off_t offset,
               int whence)
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

int ufs_stat(const char *path,
             struct ufs_stat *st)
{
    (void)path;
    (void)st;

    errno = ENOSYS;
    return -1;
}


