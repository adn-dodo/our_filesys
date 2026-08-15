#define _POSIX_C_SOURCE 200809L

#include "userfs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define IMAGE "test_processes.img"
#define IMAGE_SIZE (16ULL * 1024ULL * 1024ULL)

#define NUM_PROCESSES 4

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

static int child_work(int id)
{
    char path[64];
    char text[64];
    int fd;

    /*
     * Each child has its own UserFS state,
     * so it must mount the image.
     */
    if (ufs_mount(IMAGE) < 0) {
        perror("child ufs_mount");
        return 1;
    }

    snprintf(path, sizeof(path),
             "/process_%d", id);

    if (ufs_create(path) < 0) {
        perror("child ufs_create");
        (void)ufs_unmount();
        return 1;
    }

    fd = ufs_open(path, UFS_O_WRONLY);

    if (fd < 0) {
        perror("child ufs_open");
        (void)ufs_unmount();
        return 1;
    }

    snprintf(text, sizeof(text),
             "process-%d-data", id);

    if (ufs_write(fd, text, strlen(text)) !=
        (ssize_t)strlen(text)) {
        perror("child ufs_write");
        (void)ufs_close(fd);
        (void)ufs_unmount();
        return 1;
    }

    if (ufs_close(fd) < 0) {
        (void)ufs_unmount();
        return 1;
    }

    if (ufs_unmount() < 0) {
        perror("child ufs_unmount");
        return 1;
    }

    return 0;
}

int main(void)
{
    pid_t children[NUM_PROCESSES];
    int i;

    printf("========================================\n");
    printf(" UserFS Process Tests\n");
    printf("========================================\n");

    cleanup();

    if (ufs_format(IMAGE, IMAGE_SIZE) < 0) {
        perror("ufs_format");
        return 1;
    }

    /*
     * Parent does not need to remain mounted while children
     * perform their independent mounts.
     */
    for (i = 0; i < NUM_PROCESSES; i++) {
        children[i] = fork();

        if (children[i] < 0) {
            perror("fork");
            failed++;
            cleanup();
            return 1;
        }

        if (children[i] == 0) {
            exit(child_work(i));
        }
    }

    for (i = 0; i < NUM_PROCESSES; i++) {
        int status;

        if (waitpid(children[i], &status, 0) < 0) {
            perror("waitpid");
            failed++;
            continue;
        }

        check(WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
              "child process completed successfully");
    }

    /*
     * Remount after all children finish and verify
     * that every file survived.
     */
    check(ufs_mount(IMAGE) == 0,
          "parent remounts image");

    for (i = 0; i < NUM_PROCESSES; i++) {
        char path[64];
        struct ufs_stat st;

        snprintf(path, sizeof(path),
                 "/process_%d", i);

        check(ufs_stat(path, &st) == 0,
              "process-created file exists");

        check(st.size > 0,
              "process-created file contains data");
    }

    check(ufs_unmount() == 0,
          "parent unmounts image");

    unlink(IMAGE);

    printf("\n========================================\n");
    printf("Passed: %d\n", passed);
    printf("Failed: %d\n", failed);
    printf("========================================\n");

    return failed == 0 ? 0 : 1;
}
