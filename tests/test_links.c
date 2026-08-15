#include "userfs.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define IMAGE_PATH "links_test.img"
#define IMAGE_SIZE (16U * 1024U * 1024U)

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

static void write_text(const char *path, const char *text)
{
    int fd = ufs_open(path, UFS_O_WRONLY);
    check(fd >= 0, "open file for writing");
    check(ufs_write(fd, text, strlen(text)) == (ssize_t)strlen(text),
          "write file contents");
    check(ufs_close(fd) == 0, "close written file");
}

static void read_exact(const char *path, const char *expected)
{
    char buffer[128];
    int fd = ufs_open(path, UFS_O_RDONLY);
    ssize_t received;

    check(fd >= 0, "open path for reading");
    memset(buffer, 0, sizeof(buffer));
    received = ufs_read(fd, buffer, sizeof(buffer));
    check(received == (ssize_t)strlen(expected) &&
              memcmp(buffer, expected, strlen(expected)) == 0,
          "read expected shared contents");
    check(ufs_close(fd) == 0, "close read descriptor");
}

static void test_hard_links(void)
{
    struct ufs_stat original;
    struct ufs_stat alias;

    check(ufs_mkdir("/hard") == 0, "create hard-link directory");
    check(ufs_create("/hard/original.txt") == 0, "create original file");
    write_text("/hard/original.txt", "shared inode data");

    check(ufs_link("/hard/original.txt", "/hard/alias.txt") == 0,
          "create hard link");
    check(ufs_stat("/hard/original.txt", &original) == 0 &&
              ufs_stat("/hard/alias.txt", &alias) == 0 &&
              original.link_count == 2 && alias.link_count == 2 &&
              original.size == alias.size,
          "both hard-link names report link_count two");
    read_exact("/hard/alias.txt", "shared inode data");

    errno = 0;
    expect_error(ufs_link("/hard", "/hard/bad-directory-link"), EPERM,
                 "directory hard links are rejected");
    errno = 0;
    expect_error(ufs_link("/hard/alias.txt", "/hard/original.txt"), EEXIST,
                 "hard-link destination must not already exist");

    check(ufs_unlink("/hard/original.txt") == 0,
          "remove one hard-link name");
    errno = 0;
    expect_error(ufs_stat("/hard/original.txt", &original), ENOENT,
                 "removed hard-link name disappears");
    check(ufs_stat("/hard/alias.txt", &alias) == 0 && alias.link_count == 1,
          "remaining link keeps inode alive with link_count one");
    read_exact("/hard/alias.txt", "shared inode data");
}

static void test_symbolic_links(void)
{
    struct ufs_stat followed;
    struct ufs_stat link_info;
    char target[UFS_MAX_PATH + 1];
    char short_target[5];
    ssize_t length;

    check(ufs_mkdir("/sym") == 0, "create symbolic-link directory");
    check(ufs_create("/sym/target.txt") == 0, "create symlink target");
    write_text("/sym/target.txt", "symbolic target data");

    check(ufs_symlink("/sym/target.txt", "/sym/absolute") == 0,
          "create absolute symbolic link");
    check(ufs_symlink("target.txt", "/sym/relative") == 0,
          "create relative symbolic link");
    read_exact("/sym/absolute", "symbolic target data");
    read_exact("/sym/relative", "symbolic target data");

    memset(target, 0, sizeof(target));
    length = ufs_readlink("/sym/absolute", target, sizeof(target));
    check(length == (ssize_t)strlen("/sym/target.txt") &&
              memcmp(target, "/sym/target.txt", (size_t)length) == 0,
          "readlink returns stored target without following it");
    memset(short_target, 'X', sizeof(short_target));
    check(ufs_readlink("/sym/absolute", short_target,
                       sizeof(short_target)) == (ssize_t)sizeof(short_target) &&
              memcmp(short_target, "/sym/", sizeof(short_target)) == 0,
          "readlink safely truncates to caller buffer size");

    check(ufs_stat("/sym/absolute", &followed) == 0 &&
              followed.type == UFS_TYPE_FILE &&
              ufs_lstat("/sym/absolute", &link_info) == 0 &&
              link_info.type == UFS_TYPE_SYMLINK &&
              link_info.size == strlen("/sym/target.txt"),
          "stat follows while lstat describes the symbolic link");

    check(ufs_mkdir("/sym/sub") == 0, "create directory below symlink target");
    check(ufs_create("/sym/sub/inside") == 0, "create file below directory");
    check(ufs_symlink("/sym/sub", "/directory-link") == 0,
          "create symbolic link to directory");
    check(ufs_stat("/directory-link/inside", &followed) == 0 &&
              followed.type == UFS_TYPE_FILE,
          "resolver follows a symlink in an intermediate component");

    check(ufs_symlink("missing", "/sym/dangling") == 0,
          "create dangling symbolic link");
    errno = 0;
    expect_error(ufs_stat("/sym/dangling", &followed), ENOENT,
                 "following dangling symlink returns ENOENT");
    check(ufs_lstat("/sym/dangling", &link_info) == 0 &&
              link_info.type == UFS_TYPE_SYMLINK,
          "lstat can inspect dangling symbolic link");

    check(ufs_symlink("/sym/loop-b", "/sym/loop-a") == 0 &&
              ufs_symlink("/sym/loop-a", "/sym/loop-b") == 0,
          "create symbolic-link cycle");
    errno = 0;
    expect_error(ufs_stat("/sym/loop-a", &followed), ELOOP,
                 "symbolic-link cycle returns ELOOP");

    errno = 0;
    expect_error(ufs_readlink("/sym/target.txt", target, sizeof(target)),
                 EINVAL, "readlink rejects a regular file");
    errno = 0;
    expect_error(ufs_symlink("", "/sym/empty"), ENOENT,
                 "empty symbolic-link target is rejected");
}

static void test_remount_and_deletion(void)
{
    struct ufs_stat st;

    check(ufs_unmount() == 0, "unmount link test image");
    check(ufs_mount(IMAGE_PATH) == 0, "remount the same link test image");
    read_exact("/hard/alias.txt", "shared inode data");
    read_exact("/sym/relative", "symbolic target data");
    check(ufs_lstat("/sym/absolute", &st) == 0 &&
              st.type == UFS_TYPE_SYMLINK,
          "symbolic-link inode survives remount");

    check(ufs_unlink("/sym/absolute") == 0,
          "unlink removes symbolic link itself");
    read_exact("/sym/target.txt", "symbolic target data");
    check(ufs_unlink("/hard/alias.txt") == 0,
          "remove final hard link");
    errno = 0;
    expect_error(ufs_stat("/hard/alias.txt", &st), ENOENT,
                 "inode disappears after final hard link is removed");
    check(ufs_unmount() == 0, "final unmount");
}

int main(void)
{
    check(ufs_format(IMAGE_PATH, IMAGE_SIZE) == 0,
          "format 16 MiB link test image");
    check(ufs_mount(IMAGE_PATH) == 0, "mount link test image");
    test_hard_links();
    test_symbolic_links();
    test_remount_and_deletion();
    remove(IMAGE_PATH);
    printf("\nLink integration tests passed: %d\n", checks);
    return 0;
}
