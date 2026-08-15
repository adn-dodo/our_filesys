#define _POSIX_C_SOURCE 200809L

#include "userfs.h"
#include "journal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IMAGE "test_crash_recovery.img"
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

static void cleanup(void)
{
    (void)ufs_unmount();
    unlink(IMAGE);
}

static void test_recovery_after_normal_commit(void)
{
    struct ufs_stat before;
    struct ufs_stat after;

    printf("\n[TEST] recovery after committed operation\n");

    check(ufs_create("/file") == 0,
          "create file");

    check(ufs_stat("/file", &before) == 0,
          "stat file before recovery");

    /*
     * Simulate the mount-time recovery phase.
     *
     * A clean journal should make this a no-op.
     */
    check(ufs_journal_recover() == 0,
          "journal recovery succeeds on clean journal");

    check(ufs_stat("/file", &after) == 0,
          "file still exists after recovery");

    check(after.size == before.size,
          "file size unchanged after recovery");

    check(after.link_count == before.link_count,
          "link count unchanged after recovery");
}

static void test_remount_recovery(void)
{
    struct ufs_stat st;

    printf("\n[TEST] recovery across remount\n");

    check(ufs_create("/persistent") == 0,
          "create persistent file");

    check(ufs_unmount() == 0,
          "unmount after committed operation");

    check(ufs_mount(IMAGE) == 0,
          "remount filesystem");

    check(ufs_journal_recover() == 0,
          "run journal recovery after remount");

    check(ufs_stat("/persistent", &st) == 0,
          "persistent file exists after remount");

    check(st.type == UFS_TYPE_FILE,
          "persistent inode has file type");
}

int main(void)
{
    printf("========================================\n");
    printf(" UserFS Crash / Recovery Tests\n");
    printf("========================================\n");

    cleanup();

    if (ufs_format(IMAGE, IMAGE_SIZE) < 0) {
        perror("ufs_format");
        return 1;
    }

    if (ufs_mount(IMAGE) < 0) {
        perror("ufs_mount");
        cleanup();
        return 1;
    }

    test_recovery_after_normal_commit();

    test_remount_recovery();

    cleanup();

    printf("\n========================================\n");
    printf("Passed: %d\n", passed);
    printf("Failed: %d\n", failed);
    printf("========================================\n");

    return failed == 0 ? 0 : 1;
}
