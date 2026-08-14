#include "file_io.h"
#include "userfs.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * RAM backend used only to test Member 4 before the other modules are ready.
 * It implements the exact helper contract documented in file_io.c.
 */

#define TEST_FILE_INODE 1U
#define TEST_DIR_INODE  2U
#define MOCK_DATA_BLOCKS 8U

static int mock_mounted;
static int mock_force_enospc;
static file_io_inode_t mock_inodes[3];
static uint8_t mock_blocks[MOCK_DATA_BLOCKS][UFS_BLOCK_SIZE];
static int tests_passed;

static void reset_mock(void)
{
    uint32_t i;

    memset(mock_inodes, 0, sizeof(mock_inodes));
    memset(mock_blocks, 0, sizeof(mock_blocks));
    mock_mounted = 1;
    mock_force_enospc = 0;

    mock_inodes[TEST_FILE_INODE].type = UFS_TYPE_FILE;
    mock_inodes[TEST_FILE_INODE].single_indirect = FILE_IO_INVALID_BLOCK;
    mock_inodes[TEST_FILE_INODE].double_indirect = FILE_IO_INVALID_BLOCK;

    mock_inodes[TEST_DIR_INODE].type = UFS_TYPE_DIR;
    mock_inodes[TEST_DIR_INODE].single_indirect = FILE_IO_INVALID_BLOCK;
    mock_inodes[TEST_DIR_INODE].double_indirect = FILE_IO_INVALID_BLOCK;

    for (i = 0; i < FILE_IO_DIRECT_BLOCKS; ++i) {
        mock_inodes[TEST_FILE_INODE].direct[i] = FILE_IO_INVALID_BLOCK;
        mock_inodes[TEST_DIR_INODE].direct[i] = FILE_IO_INVALID_BLOCK;
    }

    descriptor_table_reset();
}

static void pass(const char *name)
{
    ++tests_passed;
    printf("[PASS] %s\n", name);
}

static void expect_error(int result, int expected_errno, const char *name)
{
    assert(result == -1);
    assert(errno == expected_errno);
    pass(name);
}

int ufs_is_mounted(void)
{
    return mock_mounted;
}

int resolve_path(const char *path, uint32_t *inode_number)
{
    if (path == NULL || inode_number == NULL) {
        return -EINVAL;
    }
    if (strcmp(path, "/file") == 0) {
        *inode_number = TEST_FILE_INODE;
        return 0;
    }
    if (strcmp(path, "/dir") == 0) {
        *inode_number = TEST_DIR_INODE;
        return 0;
    }
    return -ENOENT;
}

int read_inode_for_io(uint32_t inode_number, file_io_inode_t *inode)
{
    if (inode_number >= 3U || inode == NULL) {
        return -EINVAL;
    }
    *inode = mock_inodes[inode_number];
    return 0;
}

int read_block_for_io(uint32_t block_number, void *buffer)
{
    if (block_number < FILE_IO_DATA_START ||
        block_number >= FILE_IO_DATA_START + MOCK_DATA_BLOCKS ||
        buffer == NULL) {
        return -EIO;
    }
    memcpy(buffer,
           mock_blocks[block_number - FILE_IO_DATA_START],
           UFS_BLOCK_SIZE);
    return 0;
}

int write_block_for_io(uint32_t block_number, const void *buffer)
{
    if (block_number < FILE_IO_DATA_START ||
        block_number >= FILE_IO_DATA_START + MOCK_DATA_BLOCKS ||
        buffer == NULL) {
        return -EIO;
    }
    memcpy(mock_blocks[block_number - FILE_IO_DATA_START],
           buffer,
           UFS_BLOCK_SIZE);
    return 0;
}

int get_inode_data_block_for_io(const file_io_inode_t *inode,
                                uint32_t logical_block,
                                uint32_t *physical_block)
{
    if (inode == NULL || physical_block == NULL ||
        logical_block >= FILE_IO_DIRECT_BLOCKS ||
        inode->direct[logical_block] == FILE_IO_INVALID_BLOCK) {
        return -EIO;
    }
    *physical_block = inode->direct[logical_block];
    return 0;
}

int truncate_inode_for_io(uint32_t inode_number,
                          file_io_inode_t *inode,
                          uint64_t new_size)
{
    uint32_t old_blocks;
    uint32_t new_blocks;
    uint64_t position;

    if (inode_number != TEST_FILE_INODE || inode == NULL) {
        return -EINVAL;
    }

    old_blocks = inode->block_count;
    new_blocks = new_size == 0 ? 0U :
        (uint32_t)((new_size + UFS_BLOCK_SIZE - 1U) / UFS_BLOCK_SIZE);

    /* The production helper must also fail before changing metadata. */
    if (mock_force_enospc || new_blocks > MOCK_DATA_BLOCKS) {
        return -ENOSPC;
    }

    while (old_blocks < new_blocks) {
        inode->direct[old_blocks] = FILE_IO_DATA_START + old_blocks;
        memset(mock_blocks[old_blocks], 0, UFS_BLOCK_SIZE);
        ++old_blocks;
    }

    /* Zero every newly exposed byte, including a gap in an existing block. */
    for (position = inode->size; position < new_size; ++position) {
        uint32_t logical = (uint32_t)(position / UFS_BLOCK_SIZE);
        size_t inside = (size_t)(position % UFS_BLOCK_SIZE);
        mock_blocks[logical][inside] = 0;
    }

    inode->size = new_size;
    inode->block_count = new_blocks;
    mock_inodes[inode_number] = *inode;
    return 0;
}

static void test_open_and_close(void)
{
    int fd;
    int fds[UFS_MAX_OPEN_FILES];
    int i;

    reset_mock();
    fd = ufs_open("/file", UFS_O_RDWR);
    assert(fd == 0);
    assert(inode_is_open(TEST_FILE_INODE) == 1);
    pass("open existing file");

    errno = 0;
    expect_error(ufs_open("/missing", UFS_O_RDONLY), ENOENT,
                 "open missing path");
    errno = 0;
    expect_error(ufs_open("/dir", UFS_O_RDONLY), EISDIR,
                 "reject directory open");
    errno = 0;
    expect_error(ufs_open("/file", 0), EINVAL, "reject invalid flags");
    errno = 0;
    expect_error(ufs_open("/file", UFS_O_RDONLY | UFS_O_APPEND), EINVAL,
                 "reject read-only append");

    assert(ufs_close(fd) == 0);
    assert(inode_is_open(TEST_FILE_INODE) == 0);
    pass("close valid descriptor");

    errno = 0;
    expect_error(ufs_close(-1), EBADF, "reject negative descriptor");
    errno = 0;
    expect_error(ufs_close(UFS_MAX_OPEN_FILES), EBADF,
                 "reject descriptor 32");
    errno = 0;
    expect_error(ufs_close(fd), EBADF, "reject double close");

    assert(ufs_open("/file", UFS_O_RDWR) == 0);
    assert(ufs_close(0) == 0);
    pass("reuse closed descriptor");

    descriptor_table_reset();
    for (i = 0; i < UFS_MAX_OPEN_FILES; ++i) {
        fds[i] = ufs_open("/file", UFS_O_RDONLY);
        assert(fds[i] == i);
    }
    errno = 0;
    expect_error(ufs_open("/file", UFS_O_RDONLY), EMFILE,
                 "reject 33rd open");
    for (i = 0; i < UFS_MAX_OPEN_FILES; ++i) {
        assert(ufs_close(fds[i]) == 0);
    }
}

static void test_size(size_t size)
{
    uint8_t written[1500];
    uint8_t received[1500];
    int fd;
    size_t i;

    reset_mock();
    for (i = 0; i < size; ++i) {
        written[i] = (uint8_t)(i % 251U);
    }
    memset(received, 0, sizeof(received));

    fd = ufs_open("/file", UFS_O_RDWR);
    assert(fd >= 0);
    assert(ufs_write(fd, size == 0 ? NULL : written, size) == (ssize_t)size);
    assert(ufs_seek(fd, 0, SEEK_SET) == 0);
    assert(ufs_read(fd, size == 0 ? NULL : received, size) == (ssize_t)size);
    assert(memcmp(written, received, size) == 0);
    assert(ufs_close(fd) == 0);
}

static void test_read_write_sizes(void)
{
    test_size(0);
    test_size(1);
    test_size(511);
    test_size(512);
    test_size(513);
    test_size(1500);
    pass("read/write sizes 0, 1, 511, 512, 513, 1500");
}

static void test_read_rules(void)
{
    const char text[] = "abcdef";
    char buffer[16] = {0};
    int fd;

    reset_mock();
    fd = ufs_open("/file", UFS_O_RDWR);
    assert(ufs_write(fd, text, 6) == 6);
    assert(ufs_seek(fd, 0, SEEK_SET) == 0);
    assert(ufs_read(fd, buffer, 2) == 2);
    assert(ufs_read(fd, buffer + 2, 4) == 4);
    assert(memcmp(buffer, text, 6) == 0);
    pass("read advances offset");

    assert(ufs_read(fd, buffer, 1) == 0);
    pass("read at EOF returns zero");

    assert(ufs_seek(fd, -2, SEEK_END) == 4);
    assert(ufs_read(fd, buffer, 10) == 2);
    pass("read past EOF returns available bytes");
    assert(ufs_close(fd) == 0);

    fd = ufs_open("/file", UFS_O_WRONLY);
    errno = 0;
    expect_error((int)ufs_read(fd, buffer, 1), EBADF,
                 "reject read through write-only descriptor");
    assert(ufs_close(fd) == 0);

    fd = ufs_open("/file", UFS_O_RDONLY);
    errno = 0;
    expect_error((int)ufs_read(fd, NULL, 1), EINVAL,
                 "reject invalid read buffer");
    assert(ufs_read(fd, NULL, 0) == 0);
    assert(ufs_close(fd) == 0);
}

static void test_write_rules(void)
{
    char result[16] = {0};
    int fd;

    reset_mock();
    fd = ufs_open("/file", UFS_O_RDONLY);
    errno = 0;
    expect_error((int)ufs_write(fd, "x", 1), EBADF,
                 "reject write through read-only descriptor");
    assert(ufs_close(fd) == 0);

    fd = ufs_open("/file", UFS_O_RDWR);
    assert(ufs_write(fd, "abcdef", 6) == 6);
    assert(ufs_seek(fd, 2, SEEK_SET) == 2);
    assert(ufs_write(fd, "XY", 2) == 2);
    assert(ufs_seek(fd, 0, SEEK_SET) == 0);
    assert(ufs_read(fd, result, 6) == 6);
    assert(memcmp(result, "abXYef", 6) == 0);
    pass("write in middle overwrites bytes");

    assert(ufs_seek(fd, 0, SEEK_END) == 6);
    assert(ufs_write(fd, "!", 1) == 1);
    assert(mock_inodes[TEST_FILE_INODE].size == 7);
    pass("write at EOF extends file");

    assert(ufs_seek(fd, 10, SEEK_SET) == 10);
    assert(ufs_write(fd, "Z", 1) == 1);
    assert(mock_inodes[TEST_FILE_INODE].size == 11);
    assert(ufs_seek(fd, 7, SEEK_SET) == 7);
    memset(result, 0x7f, sizeof(result));
    assert(ufs_read(fd, result, 4) == 4);
    assert(result[0] == 0 && result[1] == 0 && result[2] == 0 &&
           result[3] == 'Z');
    pass("seek past EOF write zero-fills gap");
    assert(ufs_close(fd) == 0);

    reset_mock();
    fd = ufs_open("/file", UFS_O_WRONLY | UFS_O_APPEND);
    assert(ufs_write(fd, "A", 1) == 1);
    assert(ufs_seek(fd, 0, SEEK_SET) == 0);
    assert(ufs_write(fd, "B", 1) == 1);
    assert(ufs_close(fd) == 0);
    fd = ufs_open("/file", UFS_O_RDONLY);
    memset(result, 0, sizeof(result));
    assert(ufs_read(fd, result, 2) == 2);
    assert(memcmp(result, "AB", 2) == 0);
    pass("append always writes at end");
    assert(ufs_close(fd) == 0);

    reset_mock();
    fd = ufs_open("/file", UFS_O_RDWR);
    assert(ufs_write(fd, "old", 3) == 3);
    assert(ufs_seek(fd, 3, SEEK_SET) == 3);
    mock_force_enospc = 1;
    errno = 0;
    expect_error((int)ufs_write(fd, "new", 3), ENOSPC,
                 "ENOSPC returned safely");
    assert(mock_inodes[TEST_FILE_INODE].size == 3);
    assert(ufs_seek(fd, 0, SEEK_SET) == 0);
    memset(result, 0, sizeof(result));
    assert(ufs_read(fd, result, 3) == 3);
    assert(memcmp(result, "old", 3) == 0);
    pass("ENOSPC leaves original file unchanged");
    assert(ufs_close(fd) == 0);
}

static void test_seek_rules(void)
{
    int fd;
    uint64_t old_size;

    reset_mock();
    fd = ufs_open("/file", UFS_O_RDWR);
    assert(ufs_write(fd, "0123456789", 10) == 10);

    assert(ufs_seek(fd, 2, SEEK_SET) == 2);
    pass("SEEK_SET");
    assert(ufs_seek(fd, 3, SEEK_CUR) == 5);
    pass("SEEK_CUR");
    assert(ufs_seek(fd, -2, SEEK_END) == 8);
    pass("SEEK_END");

    old_size = mock_inodes[TEST_FILE_INODE].size;
    assert(ufs_seek(fd, 100, SEEK_SET) == 100);
    assert(mock_inodes[TEST_FILE_INODE].size == old_size);
    pass("seek past EOF does not change size");

    errno = 0;
    assert(ufs_seek(fd, -1, SEEK_SET) == (off_t)-1);
    assert(errno == EINVAL);
    pass("reject negative final offset");

    errno = 0;
    assert(ufs_seek(fd, 0, 12345) == (off_t)-1);
    assert(errno == EINVAL);
    pass("reject unknown whence");

    assert(ufs_close(fd) == 0);
    errno = 0;
    assert(ufs_seek(fd, 0, SEEK_SET) == (off_t)-1);
    assert(errno == EBADF);
    pass("reject seek on invalid descriptor");
}

static void test_independent_offsets(void)
{
    char first[3] = {0};
    char second[3] = {0};
    int writer;
    int fd1;
    int fd2;

    reset_mock();
    writer = ufs_open("/file", UFS_O_WRONLY);
    assert(writer >= 0);
    assert(ufs_write(writer, "abcdef", 6) == 6);
    assert(ufs_close(writer) == 0);

    fd1 = ufs_open("/file", UFS_O_RDONLY);
    fd2 = ufs_open("/file", UFS_O_RDONLY);
    assert(fd1 >= 0 && fd2 >= 0);

    assert(ufs_read(fd1, first, 2) == 2);
    assert(ufs_read(fd1, first, 1) == 1);
    assert(ufs_read(fd2, second, 2) == 2);
    assert(memcmp(second, "ab", 2) == 0);

    assert(ufs_close(fd1) == 0);
    assert(ufs_close(fd2) == 0);
    pass("two descriptors keep independent offsets");
}

static void test_multiple_append_descriptors(void)
{
    char result[4] = {0};
    int fd1;
    int fd2;
    int reader;

    reset_mock();
    fd1 = ufs_open("/file", UFS_O_WRONLY | UFS_O_APPEND);
    fd2 = ufs_open("/file", UFS_O_WRONLY | UFS_O_APPEND);
    assert(fd1 >= 0 && fd2 >= 0);

    assert(ufs_write(fd1, "A", 1) == 1);
    assert(ufs_write(fd2, "B", 1) == 1);
    assert(ufs_write(fd1, "C", 1) == 1);
    assert(ufs_close(fd1) == 0);
    assert(ufs_close(fd2) == 0);

    reader = ufs_open("/file", UFS_O_RDONLY);
    assert(reader >= 0);
    assert(ufs_read(reader, result, 3) == 3);
    assert(memcmp(result, "ABC", 3) == 0);
    assert(ufs_close(reader) == 0);
    pass("multiple append descriptors use current EOF");
}

static void test_binary_data(void)
{
    static const uint8_t written[] = {
        0x00U, 0xffU, 0x12U, 0x00U, 0x80U, 0x7fU
    };
    uint8_t received[sizeof(written)] = {0};
    int fd;

    reset_mock();
    fd = ufs_open("/file", UFS_O_RDWR);
    assert(fd >= 0);
    assert(ufs_write(fd, written, sizeof(written)) ==
           (ssize_t)sizeof(written));
    assert(ufs_seek(fd, 0, SEEK_SET) == 0);
    assert(ufs_read(fd, received, sizeof(received)) ==
           (ssize_t)sizeof(received));
    assert(memcmp(written, received, sizeof(written)) == 0);
    assert(ufs_close(fd) == 0);
    pass("binary data including zero bytes");
}

static void test_unmounted(void)
{
    reset_mock();
    mock_mounted = 0;
    errno = 0;
    expect_error(ufs_open("/file", UFS_O_RDONLY), ENODEV,
                 "reject operation while unmounted");
}

int main(void)
{
    test_open_and_close();
    test_read_write_sizes();
    test_read_rules();
    test_write_rules();
    test_seek_rules();
    test_independent_offsets();
    test_multiple_append_descriptors();
    test_binary_data();
    test_unmounted();

    printf("\nMember 4 tests passed: %d\n", tests_passed);
    return 0;
}
