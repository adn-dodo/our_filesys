#include "userfs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_IMAGE "userfs_demo.img"
#define DEMO_IMAGE_SIZE (1024U * 1024U)

static void fail(const char *operation)
{
    perror(operation);
    exit(EXIT_FAILURE);
}

static void show_directory(const char *path)
{
    struct ufs_dirent entries[16];
    int count;
    int index;

    memset(entries, 0, sizeof(entries));
    count = ufs_listdir(path, entries, 16);
    if (count < 0) {
        fail("ufs_listdir");
    }

    printf("Directory %s contains %d item(s):\n", path, count);
    for (index = 0; index < count; ++index) {
        printf("  %-20s  type=%s  size=%zu\n",
               entries[index].name,
               entries[index].type == UFS_TYPE_DIR ? "directory" : "file",
               entries[index].size);
    }
}

int main(void)
{
    static const char first_message[] =
        "Hello from our UserFS!\n"
        "This text is stored inside the disk image.\n";
    static const char appended_message[] =
        "This line was appended after opening the file again.\n";
    struct ufs_stat status;
    char buffer[256];
    ssize_t amount;
    int fd;

    printf("1. Formatting %s as a 1 MiB UserFS image...\n", DEMO_IMAGE);
    if (ufs_format(DEMO_IMAGE, DEMO_IMAGE_SIZE) != 0) {
        fail("ufs_format");
    }

    printf("2. Mounting the image...\n");
    if (ufs_mount(DEMO_IMAGE) != 0) {
        fail("ufs_mount");
    }

    printf("3. Creating /projects/demo/message.txt...\n");
    if (ufs_mkdir("/projects") != 0 ||
        ufs_mkdir("/projects/demo") != 0 ||
        ufs_create("/projects/demo/message.txt") != 0) {
        fail("create namespace");
    }

    printf("4. Opening and writing the file...\n");
    fd = ufs_open("/projects/demo/message.txt", UFS_O_RDWR);
    if (fd < 0) {
        fail("ufs_open");
    }
    amount = ufs_write(fd, first_message, strlen(first_message));
    if (amount != (ssize_t)strlen(first_message)) {
        fail("ufs_write");
    }
    if (ufs_close(fd) != 0) {
        fail("ufs_close");
    }

    show_directory("/");
    show_directory("/projects/demo");
    if (ufs_stat("/projects/demo/message.txt", &status) != 0) {
        fail("ufs_stat");
    }
    printf("File size before unmount: %zu bytes\n", status.size);

    printf("5. Unmounting the filesystem...\n");
    if (ufs_unmount() != 0) {
        fail("ufs_unmount");
    }

    printf("6. Mounting the SAME image again...\n");
    if (ufs_mount(DEMO_IMAGE) != 0) {
        fail("ufs_mount after unmount");
    }

    printf("7. Appending another line after remount...\n");
    fd = ufs_open("/projects/demo/message.txt",
                  UFS_O_WRONLY | UFS_O_APPEND);
    if (fd < 0) {
        fail("ufs_open append");
    }
    amount = ufs_write(fd, appended_message, strlen(appended_message));
    if (amount != (ssize_t)strlen(appended_message)) {
        fail("ufs_write append");
    }
    if (ufs_close(fd) != 0 || ufs_unmount() != 0) {
        fail("close or second unmount");
    }

    printf("8. Remounting once more and reading the persistent file...\n");
    if (ufs_mount(DEMO_IMAGE) != 0) {
        fail("final ufs_mount");
    }
    fd = ufs_open("/projects/demo/message.txt", UFS_O_RDONLY);
    if (fd < 0) {
        fail("ufs_open read");
    }
    memset(buffer, 0, sizeof(buffer));
    amount = ufs_read(fd, buffer, sizeof(buffer) - 1U);
    if (amount < 0) {
        fail("ufs_read");
    }
    buffer[amount] = '\0';

    printf("\nContent read from UserFS after remount:\n");
    printf("----------------------------------------\n");
    printf("%s", buffer);
    printf("----------------------------------------\n");

    if (ufs_close(fd) != 0 || ufs_unmount() != 0) {
        fail("final close or unmount");
    }

    printf("\nSUCCESS: the namespace and content survived remount.\n");
    printf("The disk image remains in %s so you can inspect it.\n", DEMO_IMAGE);
    return 0;
}
