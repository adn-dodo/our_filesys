#define _POSIX_C_SOURCE 200809L

#include "userfs.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IMAGE "test_threads.img"
#define IMAGE_SIZE (16ULL * 1024ULL * 1024ULL)

#define NUM_THREADS 8
#define APPENDS_PER_THREAD 50

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

/* =========================================================
 * Test 1:
 * Each thread creates and writes its own file.
 * ========================================================= */

struct create_arg {
    int id;
};

static void *create_worker(void *arg)
{
    struct create_arg *data = arg;
    char path[64];
    char text[128];
    int fd;

    snprintf(path, sizeof(path), "/file_%d", data->id);

    if (ufs_create(path) < 0) {
        return (void *)1;
    }

    fd = ufs_open(path, UFS_O_WRONLY);

    if (fd < 0) {
        return (void *)1;
    }

    snprintf(text, sizeof(text),
             "thread-%d-data", data->id);

    if (ufs_write(fd, text, strlen(text)) !=
        (ssize_t)strlen(text)) {
        (void)ufs_close(fd);
        return (void *)1;
    }

    if (ufs_close(fd) < 0) {
        return (void *)1;
    }

    return NULL;
}

static void test_concurrent_create(void)
{
    pthread_t threads[NUM_THREADS];
    struct create_arg args[NUM_THREADS];
    int i;

    printf("\n[TEST] concurrent create/write\n");

    for (i = 0; i < NUM_THREADS; i++) {
        args[i].id = i;

        if (pthread_create(&threads[i], NULL,
                           create_worker, &args[i]) != 0) {
            failed++;
            return;
        }
    }

    for (i = 0; i < NUM_THREADS; i++) {
        void *result = NULL;

        pthread_join(threads[i], &result);

        check(result == NULL,
              "thread created and wrote its own file");
    }

    for (i = 0; i < NUM_THREADS; i++) {
        char path[64];
        struct ufs_stat st;

        snprintf(path, sizeof(path), "/file_%d", i);

        check(ufs_stat(path, &st) == 0,
              "file exists after concurrent creation");

        check(st.size > 0,
              "file contains written data");
    }
}

/* =========================================================
 * Test 2:
 * All threads append to the same file.
 * ========================================================= */

struct append_arg {
    int id;
};

static void *append_worker(void *arg)
{
    struct append_arg *data = arg;
    int i;

    for (i = 0; i < APPENDS_PER_THREAD; i++) {
        int fd;
        char text[64];
        int length;

        fd = ufs_open("/append_file",
                      UFS_O_WRONLY | UFS_O_APPEND);

        if (fd < 0) {
            return (void *)1;
        }

        length = snprintf(text, sizeof(text),
                          "T%d\n", data->id);

        if (ufs_write(fd, text, (size_t)length) != length) {
            (void)ufs_close(fd);
            return (void *)1;
        }

        if (ufs_close(fd) < 0) {
            return (void *)1;
        }
    }

    return NULL;
}

static void test_concurrent_append(void)
{
    pthread_t threads[NUM_THREADS];
    struct append_arg args[NUM_THREADS];
    int i;

    printf("\n[TEST] concurrent append\n");

    check(ufs_create("/append_file") == 0,
          "create append file");

    for (i = 0; i < NUM_THREADS; i++) {
        args[i].id = i;

        if (pthread_create(&threads[i], NULL,
                           append_worker, &args[i]) != 0) {
            failed++;
            return;
        }
    }

    for (i = 0; i < NUM_THREADS; i++) {
        void *result = NULL;

        pthread_join(threads[i], &result);

        check(result == NULL,
              "append thread completed successfully");
    }

    {
        struct ufs_stat st;

        check(ufs_stat("/append_file", &st) == 0,
              "stat append file");

        /*
         * Every thread writes APPENDS_PER_THREAD strings.
         * Each string is at least 3 bytes: "T0\n".
         */
        check(st.size >=
              (uint64_t)(NUM_THREADS *
                         APPENDS_PER_THREAD * 3),
              "append file contains all writes");
    }
}

int main(void)
{
    printf("========================================\n");
    printf(" UserFS Thread Tests\n");
    printf("========================================\n");

    if (setup() < 0) {
        cleanup();
        return 1;
    }

    test_concurrent_create();
    test_concurrent_append();

    cleanup();

    printf("\n========================================\n");
    printf("Passed: %d\n", passed);
    printf("Failed: %d\n", failed);
    printf("========================================\n");

    return failed == 0 ? 0 : 1;
}
