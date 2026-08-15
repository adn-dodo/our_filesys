#define _POSIX_C_SOURCE 200809L

#include "userfs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define IMAGE "test_permissions.img"
#define IMAGE_SIZE (16ULL * 1024ULL * 1024ULL)

static int passed = 0;
static int failed = 0;

static void check(int condition, const char *message)
{
    if (condition) {
        printf("PASS: %s\n", message);
        passed++;
    } else {
        printf("FAIL: %s\n", message);
        failed++;
    }
}

static int timestamp_changed(struct timespec a, struct timespec b)
{
    return a.tv_sec != b.tv_sec || a.tv_nsec != b.tv_nsec;
}

static void wait_for_clock_tick(void)
{
    const struct timespec delay = {0, 1000000L};
    (void)nanosleep(&delay, NULL);
}

static int begin_unprivileged_check(void)
{
    if (geteuid() == 0 && seteuid(12345) < 0) {
        return 1;
    }
    return 0;
}

static void end_unprivileged_check(void)
{
    if (getuid() == 0 && seteuid(0) < 0) {
        perror("seteuid(0)");
        exit(1);
    }
}

static void cleanup(void)
{
    if (ufs_unmount() < 0) {
        /* It may already be unmounted. */
    }

    unlink(IMAGE);
}

static int setup(void)
{
    cleanup();

    if (ufs_format(IMAGE, IMAGE_SIZE) < 0) {
        perror("ufs_format");
        return -1;
    }

    if (ufs_mount(IMAGE) < 0) {
        perror("ufs_mount");
        return -1;
    }

    return 0;
}

/* ---------------------------------------------------------
 * 0555 directory:
 * creating inside must fail with EACCES
 * --------------------------------------------------------- */
static void test_no_write_directory(void)
{
    int result;
    int privilege_result;

    printf("\n[TEST] 0555 directory cannot create inside\n");

    check(ufs_mkdir("/readonly") == 0,
          "create readonly directory");

    check(ufs_chmod("/readonly", 0555) == 0,
          "chmod directory to 0555");

    errno = 0;
    privilege_result = begin_unprivileged_check();
    if (privilege_result != 0) {
        puts("SKIP: runtime does not permit dropping UID 0");
        return;
    }
    result = ufs_create("/readonly/file");
    {
        int saved_errno = errno;
        end_unprivileged_check();
        errno = saved_errno;
    }

    check(result == -1,
          "create inside 0555 directory fails");

    check(errno == EACCES,
          "errno is EACCES for 0555 create");
}

/* ---------------------------------------------------------
 * 0333 directory:
 * listing requires R_OK | X_OK
 * --------------------------------------------------------- */
static void test_no_read_directory(void)
{
    struct ufs_dirent entries[16];
    int result;
    int privilege_result;

    printf("\n[TEST] 0333 directory cannot be listed\n");

    check(ufs_mkdir("/writeonly") == 0,
          "create writeonly directory");

    check(ufs_chmod("/writeonly", 0333) == 0,
          "chmod directory to 0333");

    errno = 0;
    privilege_result = begin_unprivileged_check();
    if (privilege_result != 0) {
        puts("SKIP: runtime does not permit dropping UID 0");
        return;
    }
    result = ufs_listdir("/writeonly", entries, 16);
    {
        int saved_errno = errno;
        end_unprivileged_check();
        errno = saved_errno;
    }

    check(result == -1,
          "list 0333 directory fails");

    check(errno == EACCES,
          "errno is EACCES for 0333 listing");
}

/* ---------------------------------------------------------
 * 0000 directory:
 * traversal must fail with EACCES
 * --------------------------------------------------------- */
static void test_no_execute_directory(void)
{
    int result;
    int privilege_result;

    printf("\n[TEST] 0000 directory cannot be traversed\n");

    check(ufs_mkdir("/private") == 0,
          "create private directory");

    /*
     * Create the file before removing permissions.
     */
    check(ufs_create("/private/file") == 0,
          "create file inside private directory");

    check(ufs_chmod("/private", 0000) == 0,
          "chmod directory to 0000");

    errno = 0;
    result = ufs_stat("/private/file", NULL);

    /*
     * NULL stat itself causes EINVAL, so don't use this call
     * to test traversal.
     *
     * Instead use ufs_open(), which resolves the path.
     */
    (void)result;

    errno = 0;
    privilege_result = begin_unprivileged_check();
    if (privilege_result != 0) {
        puts("SKIP: runtime does not permit dropping UID 0");
        return;
    }
    result = ufs_open("/private/file", UFS_O_RDONLY);
    {
        int saved_errno = errno;
        end_unprivileged_check();
        errno = saved_errno;
    }

    check(result == -1,
          "open through 0000 directory fails");

    check(errno == EACCES,
          "errno is EACCES for 0000 traversal");
}

/* ---------------------------------------------------------
 * Parent mtime + ctime after create/remove
 * --------------------------------------------------------- */
static void test_parent_timestamps(void)
{
    struct ufs_stat before;
    struct ufs_stat after_create;
    struct ufs_stat after_remove;

    printf("\n[TEST] parent timestamps\n");

    check(ufs_mkdir("/parent") == 0,
          "create parent directory");

    check(ufs_stat("/parent", &before) == 0,
          "get initial parent timestamps");

    /*
     * Give clock_gettime() a chance to produce a different
     * timestamp on systems with coarse clocks.
     */
    wait_for_clock_tick();

    check(ufs_create("/parent/file") == 0,
          "create child file");

    check(ufs_stat("/parent", &after_create) == 0,
          "get parent timestamps after create");

    check(timestamp_changed(before.mtime, after_create.mtime),
          "parent mtime changed after create");

    check(timestamp_changed(before.ctime, after_create.ctime),
          "parent ctime changed after create");

    wait_for_clock_tick();

    check(ufs_unlink("/parent/file") == 0,
          "remove child file");

    check(ufs_stat("/parent", &after_remove) == 0,
          "get parent timestamps after remove");

    check(timestamp_changed(after_create.mtime, after_remove.mtime),
          "parent mtime changed after remove");

    check(timestamp_changed(after_create.ctime, after_remove.ctime),
          "parent ctime changed after remove");
}

/* ---------------------------------------------------------
 * Hard link:
 * target inode link count and ctime should change
 * --------------------------------------------------------- */
static void test_hardlink_ctime(void)
{
    struct ufs_stat before;
    struct ufs_stat after;
    int result;

    printf("\n[TEST] hard-link changes target ctime\n");

    check(ufs_create("/original") == 0,
          "create original file");

    check(ufs_stat("/original", &before) == 0,
          "get original metadata");

    wait_for_clock_tick();

    result = ufs_link("/original", "/second");

    check(result == 0,
          "create hard link");

    check(ufs_stat("/original", &after) == 0,
          "get metadata after hard link");

    check(after.link_count == before.link_count + 1,
          "link count increased");

    check(timestamp_changed(before.ctime, after.ctime),
          "target ctime changed after hard link");
}

/* ---------------------------------------------------------
 * Main
 * --------------------------------------------------------- */
int main(void)
{
    printf("========================================\n");
    printf(" UserFS Permission / Namespace Tests\n");
    printf("========================================\n");

    if (setup() < 0) {
        cleanup();
        return 1;
    }

    test_no_write_directory();

    /*
     * Restore permissions so later tests are not affected.
     */
    (void)ufs_chmod("/readonly", 0777);

    test_no_read_directory();

    (void)ufs_chmod("/writeonly", 0777);

    test_no_execute_directory();

    /*
     * Remove the restricted directory before continuing.
     * Depending on implementation, this may need to be done
     * while permissions are restored.
     */
    (void)ufs_chmod("/private", 0777);
    (void)ufs_unlink("/private/file");
    (void)ufs_rmdir("/private");

    test_parent_timestamps();
    test_hardlink_ctime();

    cleanup();

    printf("\n========================================\n");
    printf("Passed: %d\n", passed);
    printf("Failed: %d\n", failed);
    printf("========================================\n");

    return failed == 0 ? 0 : 1;
}
