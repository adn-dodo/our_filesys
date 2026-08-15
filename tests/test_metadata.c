#include "metadata.h"
#include "userfs.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_IMAGE "metadata_test.img"

static void test_permission_selection(void)
{
    struct ufs_inode inode;
    gid_t groups[1] = {200U};

    ufs_init_inode(&inode, UFS_TYPE_FILE);
    inode.mode = 0640U;
    inode.uid = 100U;
    inode.gid = 200U;

    assert(ufs_mode_allows(&inode, 100U, 999U, NULL, 0, R_OK | W_OK) == 0);
    errno = 0;
    assert(ufs_mode_allows(&inode, 100U, 999U, NULL, 0, X_OK) == -1);
    assert(errno == EACCES);
    assert(ufs_mode_allows(&inode, 101U, 999U, groups, 1, R_OK) == 0);
    errno = 0;
    assert(ufs_mode_allows(&inode, 101U, 999U, groups, 1, W_OK) == -1);
    assert(errno == EACCES);
    puts("[PASS] owner, group and other permission selection");
}

static void test_persistent_metadata(void)
{
    struct timespec times[2];
    struct ufs_stat before;
    struct ufs_stat after;

    assert(ufs_create("/record.txt") == 0);
    assert(ufs_stat("/record.txt", &before) == 0);
    assert((before.mode & 0777U) == 0644U);
    assert(before.uid == geteuid());
    assert(before.gid == getegid());
    assert(before.link_count == 1U);

    assert(ufs_chmod("/record.txt", 0600U) == 0);
    assert(ufs_stat("/record.txt", &after) == 0);
    assert((after.mode & 0777U) == 0600U);

    times[0].tv_sec = 1000;
    times[0].tv_nsec = 100;
    times[1].tv_sec = 2000;
    times[1].tv_nsec = 200;
    assert(ufs_utimens("/record.txt", times) == 0);
    assert(ufs_unmount() == 0);
    assert(ufs_mount(TEST_IMAGE) == 0);
    assert(ufs_stat("/record.txt", &after) == 0);
    assert((after.mode & 0777U) == 0600U);
    assert(after.atime.tv_sec == 1000 && after.atime.tv_nsec == 100);
    assert(after.mtime.tv_sec == 2000 && after.mtime.tv_nsec == 200);
    puts("[PASS] permissions, ownership and timestamps survive remount");
}

int main(void)
{
    test_permission_selection();
    assert(ufs_format(TEST_IMAGE, UFS_IMAGE_SIZE) == 0);
    assert(ufs_mount(TEST_IMAGE) == 0);
    test_persistent_metadata();
    assert(ufs_unmount() == 0);
    puts("Metadata tests passed: 2");
    return 0;
}
