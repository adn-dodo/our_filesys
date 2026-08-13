#include "userfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int passed = 0;
static int failed = 0;

static void check(int condition, const char *message)
{
    if (condition) {
        printf("  PASS: %s\n", message);
        passed++;
    } else {
        printf("  FAIL: %s\n", message);
        failed++;
    }
}

static void test_lifecycle(void)
{
    printf("\n[TEST] Format / Mount / Unmount\n");

    int ret;

    ret = ufs_format("test.img", 1024 * 1024);
    check(ret == 0, "format succeeds");

    ret = ufs_mount("test.img");
    check(ret == 0, "mount succeeds");

    ret = ufs_unmount();
    check(ret == 0, "unmount succeeds");
}

static void test_root(void)
{
    printf("\n[TEST] Root directory\n");

    struct ufs_stat st;

    int ret = ufs_mount("test.img");
    check(ret == 0, "mount succeeds");

    ret = ufs_stat("/", &st);

    check(ret == 0, "stat('/') succeeds");
    check(st.type == UFS_TYPE_DIR, "root is a directory");
  

    ret = ufs_unmount();
    check(ret == 0, "unmount succeeds");
}

static void test_root_persistence(void)
{
    printf("\n[TEST] Root persistence\n");

    struct ufs_stat st;

    int ret = ufs_mount("test.img");
    check(ret == 0, "first mount succeeds");

    ret = ufs_stat("/", &st);
    check(ret == 0, "root exists before unmount");

    ret = ufs_unmount();
    check(ret == 0, "unmount succeeds");

    ret = ufs_mount("test.img");
    check(ret == 0, "second mount succeeds");

    ret = ufs_stat("/", &st);
    check(ret == 0, "root exists after remount");
    check(st.type == UFS_TYPE_DIR, "root is still a directory");

    ret = ufs_unmount();
    check(ret == 0, "final unmount succeeds");
}

static void test_directories(void)
{
    printf("\n[TEST] Directories\n");

    int ret;

    ufs_mount("test.img");

    ret = ufs_mkdir("/docs");
    check(ret == 0, "mkdir /docs succeeds");

    ret = ufs_mkdir("/docs/course");
    check(ret == 0, "mkdir /docs/course succeeds");

    ufs_unmount();
}

static void test_directory_stat(void)
{
    printf("\n[TEST] Directory stat\n");

    struct ufs_stat st;

    ufs_mount("test.img");

    int ret = ufs_stat("/docs", &st);

    check(ret == 0, "stat /docs succeeds");
    check(st.type == UFS_TYPE_DIR, "/docs is a directory");

    ret = ufs_stat("/docs/course", &st);

    check(ret == 0, "stat /docs/course succeeds");
    check(st.type == UFS_TYPE_DIR, "/docs/course is a directory");

    ufs_unmount();
}

static void test_create_file(void)
{
    printf("\n[TEST] File creation\n");

    int ret;

    ufs_mount("test.img");

    ret = ufs_create("/docs/course/notes.txt");
    check(ret == 0, "create notes.txt succeeds");

    struct ufs_stat st;

    ret = ufs_stat("/docs/course/notes.txt", &st);

    check(ret == 0, "stat notes.txt succeeds");
    check(st.type == UFS_TYPE_FILE, "notes.txt is a regular file");

    ufs_unmount();
}

static void test_listdir(void)
{
    printf("\n[TEST] Directory listing\n");

    struct ufs_dirent entries[10];

    ufs_mount("test.img");

    int ret = ufs_listdir("/docs/course", entries, 10);

    check(ret >= 0, "listdir succeeds");

    if (ret >= 0) {
        for (int i = 0; i < ret; i++) {
            printf("    %s\n", entries[i].name);
        }
    }

    ufs_unmount();
}

static void test_duplicate_create(void)
{
    printf("\n[TEST] Duplicate file\n");

    ufs_mount("test.img");

    errno = 0;

    int ret = ufs_create("/docs/course/notes.txt");

    check(ret == -1, "duplicate create fails");
    check(errno == EEXIST, "duplicate create gives EEXIST");

    ufs_unmount();
}

static void test_missing_path(void)
{
    printf("\n[TEST] Missing path\n");

    struct ufs_stat st;

    ufs_mount("test.img");

    errno = 0;

    int ret = ufs_stat("/does/not/exist", &st);

    check(ret == -1, "stat missing path fails");
    check(errno == ENOENT, "missing path gives ENOENT");

    ufs_unmount();
}

static void test_unlink(void)
{
    printf("\n[TEST] Unlink\n");

    ufs_mount("test.img");

    int ret = ufs_unlink("/docs/course/notes.txt");

    check(ret == 0, "unlink succeeds");

    struct ufs_stat st;

    errno = 0;

    ret = ufs_stat("/docs/course/notes.txt", &st);

    check(ret == -1, "deleted file no longer exists");
    check(errno == ENOENT, "deleted file gives ENOENT");

    ufs_unmount();
}

static void test_rmdir(void)
{
    printf("\n[TEST] Remove directories\n");

    ufs_mount("test.img");

    int ret = ufs_rmdir("/docs/course");
    check(ret == 0, "rmdir /docs/course succeeds");

    ret = ufs_rmdir("/docs");
    check(ret == 0, "rmdir /docs succeeds");

    ufs_unmount();
}

static void test_nonempty_directory(void)
{
    printf("\n[TEST] Non-empty directory\n");

    ufs_mount("test.img");

    ufs_mkdir("/docs");
    ufs_create("/docs/file.txt");

    errno = 0;

    int ret = ufs_rmdir("/docs");

    check(ret == -1, "cannot remove non-empty directory");
    check(errno == ENOTEMPTY, "non-empty directory gives ENOTEMPTY");

    ufs_unmount();
}

static void test_unlink_directory(void)
{
    printf("\n[TEST] Unlink directory\n");

    ufs_mount("test.img");

    errno = 0;

    int ret = ufs_unlink("/docs");

    check(ret == -1, "unlink directory fails");
    check(errno == EISDIR, "unlink directory gives EISDIR");

    ufs_unmount();
}

static void test_rmdir_file(void)
{
    printf("\n[TEST] Rmdir file\n");

    ufs_mount("test.img");

    ufs_create("/file.txt");

    errno = 0;

    int ret = ufs_rmdir("/file.txt");

    check(ret == -1, "rmdir file fails");

    ufs_unmount();
}

static void test_open_close(void)
{
    printf("\n[TEST] Open / Close\n");

    ufs_mount("test.img");

    ufs_create("/file.txt");

    int fd = ufs_open("/file.txt", UFS_O_RDWR);

    check(fd >= 0, "open file succeeds");

    int ret = ufs_close(fd);

    check(ret == 0, "close succeeds");

    ufs_unmount();
}

static void test_read_write(void)
{
    printf("\n[TEST] Read / Write\n");

    ufs_mount("test.img");

    ufs_create("/file.txt");

    int fd = ufs_open("/file.txt", UFS_O_RDWR);

    check(fd >= 0, "open file succeeds");

    char data[] = "Hello UserFS";

    ssize_t n = ufs_write(fd, data, sizeof(data) - 1);

    check(n == sizeof(data) - 1, "write succeeds");

    off_t pos = ufs_seek(fd, 0, SEEK_SET);

    check(pos == 0, "seek to beginning succeeds");

    char buffer[20] = {0};

    n = ufs_read(fd, buffer, sizeof(data) - 1);

    check(n == sizeof(data) - 1, "read succeeds");
    check(memcmp(data, buffer, sizeof(data) - 1) == 0,
          "read data matches written data");

    ufs_close(fd);
}

static void fill_buffer(unsigned char *buf, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        buf[i] = (unsigned char)(i % 256);
    }
}

static void test_rw_size(size_t size)
{
    unsigned char write_buf[1500];
    unsigned char read_buf[1500];

  fill_buffer(write_buf, size);    memset(read_buf, 0, sizeof(read_buf));

    int fd = ufs_open("/file.txt", UFS_O_RDWR);

    check(fd >= 0, "open succeeds");

    ssize_t n = ufs_write(fd, write_buf, size);

    check(n == (ssize_t)size, "write expected number of bytes");

    ufs_seek(fd, 0, SEEK_SET);

    n = ufs_read(fd, read_buf, size);

    check(n == (ssize_t)size, "read expected number of bytes");

    check(memcmp(write_buf, read_buf, size) == 0,
          "read data matches written data");

    ufs_close(fd);
}

static void test_seek(void)
{
    printf("\n[TEST] Seek\n");

    ufs_mount("test.img");

    ufs_create("/file.txt");

    int fd = ufs_open("/file.txt", UFS_O_RDWR);

    check(fd >= 0, "open succeeds");

    if (fd < 0) {
        ufs_unmount();
        return;
    }

    char data[] = "0123456789ABCDEFGHIJ";

    ssize_t n = ufs_write(fd, data, sizeof(data) - 1);

    check(n == (ssize_t)(sizeof(data) - 1),
          "write succeeds");

    off_t pos;

    /* SEEK_SET */
    pos = ufs_seek(fd, 0, SEEK_SET);

    check(pos == 0,
          "SEEK_SET moves offset to 0");

    /* SEEK_CUR */
    pos = ufs_seek(fd, 10, SEEK_CUR);

    check(pos == 10,
          "SEEK_CUR moves offset to 10");

    /* SEEK_END */
    pos = ufs_seek(fd, -5, SEEK_END);

    check(pos == 15,
          "SEEK_END moves offset to 15");
pos = ufs_seek(fd, 10, SEEK_END);

check(pos == 30,
      "seek past EOF succeeds");
    ufs_close(fd);

    ufs_unmount();
}
static void test_append(void)
{
    printf("\n[TEST] Append\n");

    ufs_mount("test.img");

    ufs_create("/file.txt");

    int fd = ufs_open("/file.txt", UFS_O_WRONLY | UFS_O_APPEND);

    check(fd >= 0, "open with append succeeds");

    char data1[] = "Hello";
    char data2[] = " World";

    ssize_t n = ufs_write(fd, data1, sizeof(data1) - 1);

    check(n == sizeof(data1) - 1, "first write succeeds");

    n = ufs_write(fd, data2, sizeof(data2) - 1);

    check(n == sizeof(data2) - 1, "second write succeeds");

    ufs_close(fd);

    ufs_unmount();
}

static void test_descriptors(void)
{
    printf("\n[TEST] Open descriptor limit\n");

    ufs_mount("test.img");

    int fds[UFS_MAX_OPEN_FILES];

    for (int i = 0; i < UFS_MAX_OPEN_FILES; i++) {
        char path[64];

        snprintf(path, sizeof(path), "/file%d.txt", i);

        ufs_create(path);

        fds[i] = ufs_open(path, UFS_O_RDWR);

        check(fds[i] >= 0, "open descriptor succeeds");
    }

    char path[64];

    snprintf(path, sizeof(path), "/file%d.txt", UFS_MAX_OPEN_FILES);

    ufs_create(path);

    int extra = ufs_open(path, UFS_O_RDWR);

    check(extra == -1, "33rd open fails");

    for (int i = 0; i < UFS_MAX_OPEN_FILES; i++) {
        ufs_close(fds[i]);
    }

    ufs_unmount();
}

int main(void)
{
    test_lifecycle();
    test_root();
    test_root_persistence();

    test_directories();
    test_directory_stat();
    test_create_file();
    test_listdir();

    test_duplicate_create();
    test_missing_path();

    test_unlink();
    test_rmdir();

    test_read_write();
    test_seek();
    test_descriptors();

    printf("\n============================\n");
    printf("Passed: %d\n", passed);
    printf("Failed: %d\n", failed);
    printf("============================\n");

    return failed != 0;
}}
