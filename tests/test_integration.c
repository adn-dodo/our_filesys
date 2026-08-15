#include "userfs.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define IMAGE_PATH "integration.img"
#define IMAGE_SIZE (16U * 1024U * 1024U)
#define BIG_SIZE 80000U
#define FILLER_SIZE 7500000U
#define SPACE_ATTEMPT_SIZE 8000000U

static int checks;
static uint8_t big_written[BIG_SIZE];
static uint8_t big_read[BIG_SIZE];
static uint8_t space_attempt[SPACE_ATTEMPT_SIZE];

static void pass(const char *message)
{
    ++checks;
    printf("[PASS] %s\n", message);
}

static void check(int condition, const char *message)
{
    assert(condition);
    pass(message);
}

static void expect_int_error(int result, int expected_errno,
                             const char *message)
{
    assert(result == -1);
    assert(errno == expected_errno);
    pass(message);
}

static void expect_offset_error(off_t result, int expected_errno,
                                const char *message)
{
    assert(result == (off_t)-1);
    assert(errno == expected_errno);
    pass(message);
}

static void test_lifecycle_and_namespace(void)
{
    struct ufs_dirent entries[4];
    struct ufs_stat st;

    errno = 0;
    expect_int_error(ufs_open("/missing", UFS_O_RDONLY), ENODEV,
                     "operations fail while unmounted");
    errno = 0;
    expect_int_error(ufs_format(IMAGE_PATH, 1234), EINVAL,
                     "format rejects the wrong image size");

    check(ufs_format(IMAGE_PATH, IMAGE_SIZE) == 0, "format 16 MiB image");
    check(ufs_mount(IMAGE_PATH) == 0, "mount formatted image");
    check(ufs_stat("/", &st) == 0 && st.type == UFS_TYPE_DIR,
          "root directory exists");

    check(ufs_mkdir("/docs") == 0, "create /docs");
    check(ufs_mkdir("/docs/course") == 0, "create nested directory");
    errno = 0;
    expect_int_error(ufs_mkdir("/docs"), EEXIST,
                     "duplicate directory returns EEXIST");
    check(ufs_create("/docs/course/notes.bin") == 0,
          "create nested regular file");
    errno = 0;
    expect_int_error(ufs_create("/docs/course/notes.bin"), EEXIST,
                     "duplicate file returns EEXIST");

    memset(entries, 0, sizeof(entries));
    check(ufs_listdir("/docs/course", entries, 4) == 1 &&
          strcmp(entries[0].name, "notes.bin") == 0 &&
          entries[0].type == UFS_TYPE_FILE,
          "listdir returns the nested file");

    errno = 0;
    expect_int_error(ufs_create("/docs//bad"), EINVAL,
                     "double slash path is rejected");
    errno = 0;
    expect_int_error(ufs_stat("/docs/missing", &st), ENOENT,
                     "missing path returns ENOENT");
    errno = 0;
    expect_int_error(ufs_truncate("/docs", 10), EISDIR,
                     "truncate rejects a directory");
}

static void test_file_io_and_persistence(void)
{
    uint8_t expected[2002];
    uint8_t received[2002];
    uint8_t zeros[200];
    struct ufs_stat st;
    int fd;
    int first;
    int second;
    size_t index;

    for (index = 0; index < 1500; ++index) {
        expected[index] = (uint8_t)(index % 251U);
    }

    errno = 0;
    expect_int_error(ufs_open("/docs", UFS_O_RDONLY), EISDIR,
                     "open rejects a directory");
    errno = 0;
    expect_int_error(ufs_open("/missing", UFS_O_RDONLY), ENOENT,
                     "open reports missing path");
    errno = 0;
    expect_int_error(ufs_open("/docs/course/notes.bin", 0), EINVAL,
                     "open rejects invalid flags");

    fd = ufs_open("/docs/course/notes.bin", UFS_O_RDWR);
    check(fd >= 0, "open regular file read/write");
    check(ufs_write(fd, expected, 1500) == 1500,
          "write 1500 bytes across three blocks");

    check(ufs_seek(fd, 510, SEEK_SET) == 510, "seek near a block boundary");
    check(ufs_write(fd, "WXYZ", 4) == 4,
          "partial write crosses a block boundary");
    memcpy(expected + 510, "WXYZ", 4);

    check(ufs_seek(fd, 2000, SEEK_SET) == 2000, "seek past EOF");
    check(ufs_write(fd, "Q", 1) == 1, "write after a gap");
    memset(expected + 1500, 0, 500);
    expected[2000] = 'Q';
    check(ufs_close(fd) == 0, "close read/write descriptor");

    fd = ufs_open("/docs/course/notes.bin", UFS_O_WRONLY | UFS_O_APPEND);
    check(fd >= 0, "open in append mode");
    check(ufs_seek(fd, 0, SEEK_SET) == 0, "seek append descriptor to zero");
    check(ufs_write(fd, "A", 1) == 1,
          "append write still uses the current EOF");
    expected[2001] = 'A';
    check(ufs_close(fd) == 0, "close append descriptor");

    first = ufs_open("/docs/course/notes.bin", UFS_O_RDONLY);
    second = ufs_open("/docs/course/notes.bin", UFS_O_RDONLY);
    check(first >= 0 && second >= 0, "open two readers");
    check(ufs_read(first, received, 3) == 3 &&
          ufs_read(second, received + 3, 2) == 2 &&
          memcmp(received, expected, 3) == 0 &&
          memcmp(received + 3, expected, 2) == 0,
          "descriptors keep independent offsets");
    check(ufs_close(first) == 0 && ufs_close(second) == 0,
          "close both independent readers");

    check(ufs_unmount() == 0, "unmount after writes");
    check(ufs_mount(IMAGE_PATH) == 0, "remount the same image");
    fd = ufs_open("/docs/course/notes.bin", UFS_O_RDONLY);
    check(fd >= 0, "open persisted file after remount");
    memset(received, 0xff, sizeof(received));
    check(ufs_read(fd, received, sizeof(received)) ==
              (ssize_t)sizeof(received) &&
          memcmp(received, expected, sizeof(expected)) == 0,
          "file data and zero-filled gap survive remount");
    check(ufs_close(fd) == 0, "close persisted file");
    check(ufs_stat("/docs/course/notes.bin", &st) == 0 && st.size == 2002,
          "persisted file size is correct");

    check(ufs_truncate("/docs/course/notes.bin", 1000) == 0,
          "truncate shrinks a file");
    check(ufs_truncate("/docs/course/notes.bin", 1200) == 0,
          "truncate grows a file");
    fd = ufs_open("/docs/course/notes.bin", UFS_O_RDONLY);
    check(ufs_seek(fd, 1000, SEEK_SET) == 1000, "seek to regrown range");
    memset(zeros, 0xff, sizeof(zeros));
    check(ufs_read(fd, zeros, sizeof(zeros)) == (ssize_t)sizeof(zeros),
          "read regrown range");
    for (index = 0; index < sizeof(zeros); ++index) {
        assert(zeros[index] == 0);
    }
    pass("regrown range is zero-filled");
    check(ufs_close(fd) == 0, "close truncated file");
}

static void test_indirect_blocks(void)
{
    int fd;
    size_t index;

    check(ufs_create("/big.bin") == 0, "create large file");
    for (index = 0; index < BIG_SIZE; ++index) {
        big_written[index] = (uint8_t)((index * 17U + 3U) % 251U);
    }

    fd = ufs_open("/big.bin", UFS_O_RDWR);
    check(fd >= 0, "open large file");
    check(ufs_write(fd, big_written, BIG_SIZE) == (ssize_t)BIG_SIZE,
          "write through direct, single-indirect, and double-indirect blocks");
    check(ufs_seek(fd, 0, SEEK_SET) == 0, "rewind large file");
    memset(big_read, 0, sizeof(big_read));
    check(ufs_read(fd, big_read, BIG_SIZE) == (ssize_t)BIG_SIZE &&
          memcmp(big_written, big_read, BIG_SIZE) == 0,
          "read and compare large binary file");
    check(ufs_close(fd) == 0, "close large file");

    check(ufs_unmount() == 0, "unmount large-file image");
    check(ufs_mount(IMAGE_PATH) == 0, "remount large-file image");
    fd = ufs_open("/big.bin", UFS_O_RDONLY);
    check(fd >= 0, "open large file after remount");
    memset(big_read, 0, sizeof(big_read));
    check(ufs_read(fd, big_read, BIG_SIZE) == (ssize_t)BIG_SIZE &&
          memcmp(big_written, big_read, BIG_SIZE) == 0,
          "indirect block mappings survive remount");
    check(ufs_close(fd) == 0, "close remounted large file");
}

static void test_enospc_rollback(void)
{
    struct ufs_stat st;
    char old_data[4] = {0};
    int fd;
    int filler_fd;

    check(ufs_create("/filler.bin") == 0, "create disk-space filler");
    filler_fd = ufs_open("/filler.bin", UFS_O_WRONLY);
    check(filler_fd >= 0, "open disk-space filler");
    check(ufs_write(filler_fd, space_attempt, FILLER_SIZE) ==
              (ssize_t)FILLER_SIZE,
          "consume enough blocks to exercise ENOSPC");
    check(ufs_close(filler_fd) == 0, "close disk-space filler");

    check(ufs_create("/space.bin") == 0, "create ENOSPC test file");
    fd = ufs_open("/space.bin", UFS_O_RDWR);
    check(fd >= 0, "open ENOSPC test file");
    check(ufs_write(fd, "old", 3) == 3, "write original ENOSPC test data");

    errno = 0;
    expect_int_error((int)ufs_write(fd, space_attempt,
                                    sizeof(space_attempt)),
                     ENOSPC, "oversized write returns ENOSPC");
    check(ufs_stat("/space.bin", &st) == 0 && st.size == 3,
          "ENOSPC preserves the original file size");
    check(ufs_seek(fd, 0, SEEK_SET) == 0 &&
          ufs_read(fd, old_data, 3) == 3 &&
          memcmp(old_data, "old", 3) == 0,
          "ENOSPC preserves the original file content");
    check(ufs_close(fd) == 0 && ufs_unlink("/space.bin") == 0,
          "close and remove ENOSPC test file");
    check(ufs_unlink("/filler.bin") == 0, "remove disk-space filler");
}

static void test_errors_descriptors_and_cleanup(void)
{
    struct ufs_stat st;
    int descriptors[UFS_MAX_OPEN_FILES];
    int fd;
    int index;
    char byte;

    fd = ufs_open("/docs/course/notes.bin", UFS_O_WRONLY);
    check(fd >= 0, "open write-only descriptor");
    errno = 0;
    expect_int_error((int)ufs_read(fd, &byte, 1), EBADF,
                     "read rejects write-only descriptor");
    check(ufs_close(fd) == 0, "close write-only descriptor");

    fd = ufs_open("/docs/course/notes.bin", UFS_O_RDONLY);
    check(fd >= 0, "open read-only descriptor");
    errno = 0;
    expect_int_error((int)ufs_write(fd, "x", 1), EBADF,
                     "write rejects read-only descriptor");
    errno = 0;
    expect_offset_error(ufs_seek(fd, -1, SEEK_SET), EINVAL,
                        "seek rejects a negative final offset");
    errno = 0;
    expect_int_error(ufs_unlink("/docs/course/notes.bin"), EBUSY,
                     "unlink rejects an open file");
    check(ufs_close(fd) == 0, "close file before unlink");

    for (index = 0; index < UFS_MAX_OPEN_FILES; ++index) {
        descriptors[index] = ufs_open("/big.bin", UFS_O_RDONLY);
        assert(descriptors[index] == index);
    }
    pass("all 32 descriptor slots can be used");
    errno = 0;
    expect_int_error(ufs_open("/big.bin", UFS_O_RDONLY), EMFILE,
                     "33rd open returns EMFILE");
    for (index = 0; index < UFS_MAX_OPEN_FILES; ++index) {
        assert(ufs_close(descriptors[index]) == 0);
    }
    pass("all descriptor slots close cleanly");

    errno = 0;
    expect_int_error(ufs_rmdir("/docs/course"), ENOTEMPTY,
                     "rmdir rejects a non-empty directory");
    check(ufs_unlink("/docs/course/notes.bin") == 0,
          "unlink regular file");
    check(ufs_unlink("/big.bin") == 0,
          "unlink large file and reclaim its blocks");
    check(ufs_rmdir("/docs/course") == 0, "remove empty nested directory");
    check(ufs_rmdir("/docs") == 0, "remove empty parent directory");

    check(ufs_unmount() == 0, "unmount after cleanup");
    check(ufs_mount(IMAGE_PATH) == 0, "remount after cleanup");
    errno = 0;
    expect_int_error(ufs_stat("/docs", &st), ENOENT,
                     "directory deletion survives remount");
    errno = 0;
    expect_int_error(ufs_stat("/big.bin", &st), ENOENT,
                     "file deletion survives remount");
    check(ufs_unmount() == 0, "final unmount");
}

int main(void)
{
    (void)remove(IMAGE_PATH);
    test_lifecycle_and_namespace();
    test_file_io_and_persistence();
    test_indirect_blocks();
    test_enospc_rollback();
    test_errors_descriptors_and_cleanup();
    (void)remove(IMAGE_PATH);

    printf("\nIntegration checks passed: %d\n", checks);
    return 0;
}
