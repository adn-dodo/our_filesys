#include "userfs.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define IMAGE_PATH "mmap_test.img"
#define IMAGE_SIZE (16U * 1024U * 1024U)
#define DATA_SIZE 700U

static int checks;

static void check(int condition, const char *message)
{
    assert(condition);
    ++checks;
    printf("[PASS] %s\n", message);
}

static void expect_error(int result, int expected_errno, const char *message)
{
    assert(result == -1);
    assert(errno == expected_errno);
    ++checks;
    printf("[PASS] %s\n", message);
}

static void expect_map_error(void *result, int expected_errno,
                             const char *message)
{
    assert(result == UFS_MAP_FAILED);
    assert(errno == expected_errno);
    ++checks;
    printf("[PASS] %s\n", message);
}

static void read_file(const char *path, uint8_t *buffer, size_t size)
{
    int fd = ufs_open(path, UFS_O_RDONLY);
    check(fd >= 0, "open file for verification");
    check(ufs_read(fd, buffer, size) == (ssize_t)size,
          "read expected verification size");
    check(ufs_close(fd) == 0, "close verification descriptor");
}

int main(void)
{
    uint8_t original[DATA_SIZE];
    uint8_t received[800];
    void *mappings[32];
    struct ufs_stat status;
    uint8_t *shared;
    uint8_t *private_mapping;
    uint8_t *offset_mapping;
    uint8_t *eof_mapping;
    int fd;
    int read_only_fd;
    int index;

    for (index = 0; index < (int)DATA_SIZE; ++index) {
        original[index] = (uint8_t)(index % 251);
    }

    check(ufs_format(IMAGE_PATH, IMAGE_SIZE) == 0,
          "format mmap test image");
    check(ufs_mount(IMAGE_PATH) == 0, "mount mmap test image");
    check(ufs_create("/mapped.bin") == 0, "create mapped file");
    fd = ufs_open("/mapped.bin", UFS_O_RDWR);
    check(fd >= 0, "open mapped file read/write");
    check(ufs_write(fd, original, sizeof(original)) ==
              (ssize_t)sizeof(original),
          "write initial mapping data across a block boundary");

    errno = 0;
    expect_map_error(ufs_mmap(-1, 10, UFS_PROT_READ, UFS_MAP_PRIVATE, 0),
                     EBADF, "mmap rejects invalid descriptor");
    errno = 0;
    expect_map_error(ufs_mmap(fd, 0, UFS_PROT_READ, UFS_MAP_PRIVATE, 0),
                     EINVAL, "mmap rejects zero length");
    errno = 0;
    expect_map_error(ufs_mmap(fd, 10, UFS_PROT_WRITE, UFS_MAP_SHARED, 0),
                     EINVAL, "mmap requires readable memory protection");

    shared = ufs_mmap(fd, DATA_SIZE,
                      UFS_PROT_READ | UFS_PROT_WRITE,
                      UFS_MAP_SHARED, 0);
    check(shared != UFS_MAP_FAILED, "create writable shared mapping");
    check(memcmp(shared, original, DATA_SIZE) == 0,
          "shared mapping initially contains file data");
    shared[0] = 'A';
    shared[511] = 'B';
    shared[512] = 'C';
    shared[699] = 'D';
    check(ufs_close(fd) == 0,
          "mapping remains valid after its descriptor is closed");
    check(ufs_msync(shared, DATA_SIZE) == 0,
          "msync writes shared mapping to UserFS");
    errno = 0;
    expect_error(ufs_unmount(), EBUSY,
                 "unmount rejects an active mapping");
    errno = 0;
    expect_error(ufs_unlink("/mapped.bin"), EBUSY,
                 "unlink rejects a mapped inode");
    check(ufs_munmap(shared, DATA_SIZE) == 0,
          "munmap releases shared mapping");

    memset(received, 0, sizeof(received));
    read_file("/mapped.bin", received, DATA_SIZE);
    check(received[0] == 'A' && received[511] == 'B' &&
              received[512] == 'C' && received[699] == 'D',
          "shared modifications crossed the 512-byte boundary");

    read_only_fd = ufs_open("/mapped.bin", UFS_O_RDONLY);
    check(read_only_fd >= 0, "open file read-only for private mapping");
    errno = 0;
    expect_map_error(ufs_mmap(read_only_fd, DATA_SIZE,
                              UFS_PROT_READ | UFS_PROT_WRITE,
                              UFS_MAP_SHARED, 0),
                     EACCES,
                     "read-only descriptor rejects writable shared mapping");
    private_mapping = ufs_mmap(read_only_fd, DATA_SIZE,
                               UFS_PROT_READ | UFS_PROT_WRITE,
                               UFS_MAP_PRIVATE, 0);
    check(private_mapping != UFS_MAP_FAILED,
          "read-only descriptor allows writable private copy");
    private_mapping[0] = 'P';
    check(ufs_msync(private_mapping, DATA_SIZE) == 0,
          "msync on private mapping is a successful no-op");
    check(ufs_munmap(private_mapping, DATA_SIZE) == 0,
          "release private mapping");
    check(ufs_close(read_only_fd) == 0, "close private-map descriptor");
    memset(received, 0, sizeof(received));
    read_file("/mapped.bin", received, DATA_SIZE);
    check(received[0] == 'A', "private mapping did not modify the file");

    fd = ufs_open("/mapped.bin", UFS_O_RDWR);
    check(fd >= 0, "reopen file for offset mapping");
    offset_mapping = ufs_mmap(fd, 20,
                              UFS_PROT_READ | UFS_PROT_WRITE,
                              UFS_MAP_SHARED, 505);
    check(offset_mapping != UFS_MAP_FAILED,
          "create nonzero-offset mapping across block boundary");
    offset_mapping[6] = 'X';
    offset_mapping[7] = 'Y';
    check(ufs_munmap(offset_mapping, 20) == 0,
          "munmap automatically synchronizes shared mapping");

    eof_mapping = ufs_mmap(fd, 150,
                           UFS_PROT_READ | UFS_PROT_WRITE,
                           UFS_MAP_SHARED, 650);
    check(eof_mapping != UFS_MAP_FAILED,
          "create mapping that extends beyond EOF");
    check(eof_mapping[50] == 0 && eof_mapping[149] == 0,
          "bytes beyond EOF begin as zero");
    eof_mapping[149] = 'Z';
    check(ufs_msync(eof_mapping, 150) == 0,
          "sync mapping beyond EOF extends the file");
    check(ufs_munmap(eof_mapping, 150) == 0,
          "release EOF-extending mapping");
    check(ufs_close(fd) == 0, "close offset-map descriptor");
    check(ufs_stat("/mapped.bin", &status) == 0 && status.size == 800,
          "shared mapping correctly extended file size");

    memset(received, 0xff, sizeof(received));
    read_file("/mapped.bin", received, sizeof(received));
    check(received[511] == 'X' && received[512] == 'Y' &&
              received[799] == 'Z',
          "offset and EOF mapping changes reached correct bytes");

    read_only_fd = ufs_open("/mapped.bin", UFS_O_RDONLY);
    check(read_only_fd >= 0, "open descriptor for mapping-table test");
    for (index = 0; index < 32; ++index) {
        mappings[index] = ufs_mmap(read_only_fd, 1, UFS_PROT_READ,
                                   UFS_MAP_PRIVATE, index);
        assert(mappings[index] != UFS_MAP_FAILED);
    }
    check(1, "allocate all 32 mapping-table entries");
    errno = 0;
    expect_map_error(ufs_mmap(read_only_fd, 1, UFS_PROT_READ,
                              UFS_MAP_PRIVATE, 40),
                     ENOMEM, "33rd mapping returns ENOMEM");
    errno = 0;
    expect_error(ufs_msync((void *)(uintptr_t)0x1234, 1), EINVAL,
                 "msync rejects unknown address");
    for (index = 0; index < 32; ++index) {
        assert(ufs_munmap(mappings[index], 1) == 0);
    }
    check(1, "release all mapping-table entries");
    check(ufs_close(read_only_fd) == 0,
          "close mapping-table descriptor");

    check(ufs_unmount() == 0, "unmount after mmap operations");
    check(ufs_mount(IMAGE_PATH) == 0, "remount mmap image");
    memset(received, 0, sizeof(received));
    read_file("/mapped.bin", received, sizeof(received));
    check(received[0] == 'A' && received[511] == 'X' &&
              received[512] == 'Y' && received[799] == 'Z',
          "mmap changes survive unmount and remount");
    check(ufs_unmount() == 0, "final mmap test unmount");
    remove(IMAGE_PATH);

    printf("\nMemory-mapping tests passed: %d\n", checks);
    return 0;
}
