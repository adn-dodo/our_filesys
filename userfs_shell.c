#include "userfs.h"
#include "ufs_internal.h"
#include "userfs_storage.h"
#include "namespace.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_IMAGE "userfs_live.img"
#define IMAGE_SIZE (1024U * 1024U)
#define INPUT_SIZE 1024U

static int mounted;
static const char *image_path;
static char current_directory[UFS_MAX_PATH + 1] = "/";

static void print_error(const char *operation)
{
    printf("ERROR: %s: %s\n", operation, strerror(errno));
}

static int require_shell_mount(void)
{
    if (!mounted) {
        printf("ERROR: first run 'mount' (or 'format', then 'mount').\n");
        return 0;
    }
    return 1;
}

static char *skip_spaces(char *text)
{
    while (text != NULL && isspace((unsigned char)*text)) {
        ++text;
    }
    return text;
}

/*
 * Convert shell paths such as "notes.txt", "../demo", and "./file" into the
 * absolute canonical paths required by the public UserFS API.
 */
static int normalize_shell_path(const char *input,
                                char output[UFS_MAX_PATH + 1])
{
    size_t position = 0;
    size_t used;

    if (input == NULL || *input == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (input[0] == '/') {
        output[0] = '/';
        output[1] = '\0';
        used = 1;
    } else {
        (void)strcpy(output, current_directory);
        used = strlen(output);
    }

    while (input[position] != '\0') {
        size_t start;
        size_t length;

        while (input[position] == '/') {
            ++position;
        }
        if (input[position] == '\0') {
            break;
        }
        start = position;
        while (input[position] != '/' && input[position] != '\0') {
            ++position;
        }
        length = position - start;

        if (length == 1 && input[start] == '.') {
            continue;
        }
        if (length == 2 && input[start] == '.' && input[start + 1] == '.') {
            if (used > 1) {
                while (used > 1 && output[used - 1] != '/') {
                    --used;
                }
                if (used > 1) {
                    --used;
                }
                output[used] = '\0';
            }
            continue;
        }
        if (length > UFS_MAX_NAME) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (used + (used > 1 ? 1U : 0U) + length > UFS_MAX_PATH) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (used > 1) {
            output[used++] = '/';
        }
        (void)memcpy(output + used, input + start, length);
        used += length;
        output[used] = '\0';
    }
    return 0;
}

static int resolve_shell_path(const char *input, int default_to_cwd,
                              char output[UFS_MAX_PATH + 1])
{
    const char *selected = input;

    if (selected == NULL && default_to_cwd) {
        selected = current_directory;
    }
    if (normalize_shell_path(selected, output) != 0) {
        print_error("path");
        return -1;
    }
    return 0;
}

static void show_help(void)
{
    printf("\nPaths may be absolute (/docs/file) or relative (../file).\n");
    printf("\nFilesystem commands (use the public UserFS API):\n");
    printf("  format                       create a fresh 1 MiB image\n");
    printf("  mount                        mount the image\n");
    printf("  unmount                      persist and close the image\n");
    printf("  pwd                          print the current UserFS directory\n");
    printf("  cd [PATH]                    change directory (default: /)\n");
    printf("  mkdir PATH                   create a directory\n");
    printf("  rmdir PATH                   remove an empty directory\n");
    printf("  create PATH                  create an empty file\n");
    printf("  touch PATH                   alias for create\n");
    printf("  write PATH TEXT              replace file content\n");
    printf("  append PATH TEXT             append to a file\n");
    printf("  cat PATH                     print file content\n");
    printf("  ls [PATH]                    list a directory (default: current)\n");
    printf("  stat [PATH]                  show public metadata\n");
    printf("  truncate PATH SIZE           resize a file\n");
    printf("  rm PATH                      delete a regular file\n");
    printf("\nInspection commands (demonstration/debug only):\n");
    printf("  inode PATH                   show the inode and block pointers\n");
    printf("  map PATH                     show logical -> physical blocks\n");
    printf("  show PATH                    show one compact file summary\n");
    printf("  bitmap                       show allocated inode/data bits\n");
    printf("  super                        show superblock/free counters\n");
    printf("\nOther commands:\n");
    printf("  help                         show this help\n");
    printf("  quit                         unmount if needed and exit\n\n");
}

static void command_format(void)
{
    if (mounted) {
        printf("ERROR: unmount before formatting.\n");
        return;
    }
    if (ufs_format(image_path, IMAGE_SIZE) != 0) {
        print_error("format");
        return;
    }
    (void)strcpy(current_directory, "/");
    printf("OK: formatted %s (1 MiB, 2048 blocks).\n", image_path);
}

static void command_mount(void)
{
    if (mounted) {
        printf("ERROR: image is already mounted.\n");
        return;
    }
    if (ufs_mount(image_path) != 0) {
        print_error("mount");
        return;
    }
    mounted = 1;
    (void)strcpy(current_directory, "/");
    printf("OK: mounted %s.\n", image_path);
}

static void command_unmount(void)
{
    if (!mounted) {
        printf("ERROR: no image is mounted.\n");
        return;
    }
    if (ufs_unmount() != 0) {
        print_error("unmount");
        return;
    }
    mounted = 0;
    (void)strcpy(current_directory, "/");
    printf("OK: unmounted; successful changes are persistent.\n");
}

static void command_pwd(void)
{
    if (require_shell_mount()) {
        printf("%s\n", current_directory);
    }
}

static void command_cd(const char *path)
{
    char resolved[UFS_MAX_PATH + 1];
    struct ufs_stat status;

    if (!require_shell_mount()) {
        return;
    }
    if (path == NULL) {
        path = "/";
    }
    if (resolve_shell_path(path, 0, resolved) != 0) {
        return;
    }
    if (ufs_stat(resolved, &status) != 0) {
        print_error("cd");
        return;
    }
    if (status.type != UFS_TYPE_DIR) {
        errno = ENOTDIR;
        print_error("cd");
        return;
    }
    (void)strcpy(current_directory, resolved);
}

static void command_ls(const char *path)
{
    char resolved[UFS_MAX_PATH + 1];
    struct ufs_dirent entries[64];
    int count;
    int index;

    if (!require_shell_mount()) {
        return;
    }
    if (resolve_shell_path(path, 1, resolved) != 0) {
        return;
    }
    memset(entries, 0, sizeof(entries));
    count = ufs_listdir(resolved, entries, 64);
    if (count < 0) {
        print_error("ls");
        return;
    }
    printf("Directory: %s\n", resolved);
    printf("NAME                             TYPE       SIZE\n");
    printf("-------------------------------- --------- --------\n");
    for (index = 0; index < count; ++index) {
        printf("%-32s %-9s %zu\n", entries[index].name,
               entries[index].type == UFS_TYPE_DIR ? "directory" : "file",
               entries[index].size);
    }
    printf("%d item(s)\n", count);
}

static void command_stat(const char *path)
{
    char resolved[UFS_MAX_PATH + 1];
    struct ufs_stat status;

    if (!require_shell_mount()) {
        return;
    }
    if (resolve_shell_path(path, 1, resolved) != 0) {
        return;
    }
    if (ufs_stat(resolved, &status) != 0) {
        print_error("stat");
        return;
    }
    printf("type=%s size=%zu bytes\n",
           status.type == UFS_TYPE_DIR ? "directory" : "file", status.size);
}

static void command_create(const char *command, const char *path)
{
    char resolved[UFS_MAX_PATH + 1];
    int result;

    if (!require_shell_mount() || path == NULL) {
        if (path == NULL) {
            printf("Usage: %s PATH\n", command);
        }
        return;
    }
    if (resolve_shell_path(path, 0, resolved) != 0) {
        return;
    }
    if (strcmp(command, "mkdir") == 0) {
        result = ufs_mkdir(resolved);
    } else {
        result = ufs_create(resolved);
    }
    if (result != 0) {
        print_error(command);
        return;
    }
    printf("OK: %s created.\n", resolved);
}

static void command_remove(const char *command, const char *path)
{
    char resolved[UFS_MAX_PATH + 1];
    int result;

    if (!require_shell_mount() || path == NULL) {
        if (path == NULL) {
            printf("Usage: %s PATH\n", command);
        }
        return;
    }
    if (resolve_shell_path(path, 0, resolved) != 0) {
        return;
    }
    if (strcmp(command, "rmdir") == 0 &&
        strcmp(resolved, current_directory) == 0) {
        errno = EBUSY;
        print_error("rmdir");
        return;
    }
    result = strcmp(command, "rmdir") == 0 ?
        ufs_rmdir(resolved) : ufs_unlink(resolved);
    if (result != 0) {
        print_error(command);
        return;
    }
    printf("OK: %s removed.\n", resolved);
}

static void command_write(const char *path, const char *text, int append)
{
    char resolved[UFS_MAX_PATH + 1];
    int flags = UFS_O_WRONLY;
    int fd;
    ssize_t amount;
    size_t length;

    if (!require_shell_mount() || path == NULL || text == NULL) {
        printf("Usage: %s PATH TEXT\n", append ? "append" : "write");
        return;
    }
    if (resolve_shell_path(path, 0, resolved) != 0) {
        return;
    }
    if (!append && ufs_truncate(resolved, 0) != 0) {
        print_error("truncate before write");
        return;
    }
    if (append) {
        flags |= UFS_O_APPEND;
    }
    fd = ufs_open(resolved, flags);
    if (fd < 0) {
        print_error("open for write");
        return;
    }
    length = strlen(text);
    amount = ufs_write(fd, text, length);
    if (amount != (ssize_t)length) {
        print_error("write");
        (void)ufs_close(fd);
        return;
    }
    if (ufs_close(fd) != 0) {
        print_error("close");
        return;
    }
    printf("OK: wrote %zu byte(s) to %s.\n", length, resolved);
}

static void command_cat(const char *path)
{
    char resolved[UFS_MAX_PATH + 1];
    uint8_t buffer[256];
    int fd;

    if (!require_shell_mount() || path == NULL) {
        printf("Usage: cat PATH\n");
        return;
    }
    if (resolve_shell_path(path, 0, resolved) != 0) {
        return;
    }
    fd = ufs_open(resolved, UFS_O_RDONLY);
    if (fd < 0) {
        print_error("cat open");
        return;
    }
    printf("----- %s -----\n", resolved);
    for (;;) {
        ssize_t amount = ufs_read(fd, buffer, sizeof(buffer));
        if (amount < 0) {
            print_error("cat read");
            (void)ufs_close(fd);
            return;
        }
        if (amount == 0) {
            break;
        }
        (void)fwrite(buffer, 1, (size_t)amount, stdout);
    }
    printf("\n------------------------------\n");
    if (ufs_close(fd) != 0) {
        print_error("cat close");
    }
}

static void command_truncate(const char *path, const char *size_text)
{
    char resolved[UFS_MAX_PATH + 1];
    char *end;
    unsigned long long requested;

    if (!require_shell_mount() || path == NULL || size_text == NULL) {
        printf("Usage: truncate PATH SIZE\n");
        return;
    }
    if (resolve_shell_path(path, 0, resolved) != 0) {
        return;
    }
    errno = 0;
    requested = strtoull(size_text, &end, 10);
    if (errno != 0 || *size_text == '\0' || *end != '\0' ||
        requested > (unsigned long long)SIZE_MAX) {
        printf("ERROR: SIZE must be a non-negative decimal number.\n");
        return;
    }
    if (ufs_truncate(resolved, (size_t)requested) != 0) {
        print_error("truncate");
        return;
    }
    printf("OK: %s is now %llu byte(s).\n", resolved, requested);
}

static int load_inode(const char *path, uint32_t *number,
                      struct ufs_inode *inode)
{
    char resolved[UFS_MAX_PATH + 1];

    if (!require_shell_mount()) {
        return -1;
    }
    if (resolve_shell_path(path, 1, resolved) != 0) {
        return -1;
    }
    if (resolve_path(resolved, number) != 0 || read_inode(*number, inode) != 0) {
        print_error("load inode");
        return -1;
    }
    return 0;
}

static void print_pointer(const char *label, uint32_t block)
{
    if (block == UFS_INVALID_BLOCK) {
        printf("  %-18s unused\n", label);
    } else {
        printf("  %-18s %u\n", label, (unsigned int)block);
    }
}

static void command_inode(const char *path)
{
    struct ufs_inode inode;
    uint32_t number;
    uint32_t index;

    if (load_inode(path, &number, &inode) != 0) {
        return;
    }
    printf("inode number: %u\n", (unsigned int)number);
    printf("type:         %s (%u)\n",
           inode.type == UFS_TYPE_DIR ? "directory" : "file",
           (unsigned int)inode.type);
    printf("size:         %llu byte(s)\n", (unsigned long long)inode.size);
    printf("data blocks:  %u\n", (unsigned int)inode.block_count);
    for (index = 0; index < UFS_DIRECT_BLOCKS; ++index) {
        char label[24];
        (void)snprintf(label, sizeof(label), "direct[%u]", (unsigned int)index);
        print_pointer(label, inode.direct[index]);
    }
    print_pointer("single_indirect", inode.single_indirect);
    print_pointer("double_indirect", inode.double_indirect);
}

static void command_map(const char *path)
{
    struct ufs_inode inode;
    uint32_t number;
    uint32_t logical;

    if (load_inode(path, &number, &inode) != 0) {
        return;
    }
    printf("inode %u: logical block -> physical image block\n",
           (unsigned int)number);
    if (inode.block_count == 0) {
        printf("  no data blocks allocated\n");
        return;
    }
    for (logical = 0; logical < inode.block_count; ++logical) {
        uint32_t physical;
        if (get_inode_data_block(&inode, logical, &physical) != 0) {
            print_error("block mapping");
            return;
        }
        printf("  logical %-5u -> physical %u\n",
               (unsigned int)logical, (unsigned int)physical);
    }
}

static void command_show(const char *path)
{
    char resolved[UFS_MAX_PATH + 1];
    struct ufs_inode inode;
    uint8_t block_bitmap[UFS_BLOCK_SIZE];
    uint8_t buffer[256];
    uint32_t number;
    uint32_t logical;
    int fd;

    if (resolve_shell_path(path, 1, resolved) != 0) {
        return;
    }
    if (load_inode(resolved, &number, &inode) != 0) {
        return;
    }
    if (inode.type != UFS_TYPE_FILE) {
        errno = EISDIR;
        print_error("show");
        return;
    }
    if (read_block_bitmap(block_bitmap) != 0) {
        print_error("read bitmap");
        return;
    }

    printf("File size       = %llu bytes\n",
           (unsigned long long)inode.size);
    printf("Data blocks     = %u\n", (unsigned int)inode.block_count);

    for (logical = 0;
         logical < inode.block_count && logical < UFS_DIRECT_BLOCKS;
         ++logical) {
        printf("direct[%u]       = %u\n", (unsigned int)logical,
               (unsigned int)inode.direct[logical]);
    }

    printf("Block map       = ");
    if (inode.block_count == 0) {
        printf("none\n");
    } else {
        for (logical = 0; logical < inode.block_count; ++logical) {
            uint32_t physical;
            if (get_inode_data_block(&inode, logical, &physical) != 0) {
                print_error("block mapping");
                return;
            }
            if (logical != 0) {
                printf(", ");
            }
            printf("logical %u → physical %u",
                   (unsigned int)logical, (unsigned int)physical);
        }
        printf("\n");
    }

    printf("Bitmap includes = ");
    if (inode.block_count == 0) {
        printf("none\n");
    } else {
        for (logical = 0; logical < inode.block_count; ++logical) {
            uint32_t physical;
            if (get_inode_data_block(&inode, logical, &physical) != 0) {
                print_error("block mapping");
                return;
            }
            if (logical != 0) {
                printf(" ");
            }
            if (bitmap_test(block_bitmap, physical)) {
                printf("%u", (unsigned int)physical);
            } else {
                printf("%u(missing)", (unsigned int)physical);
            }
        }
        printf("\n");
    }

    printf("File content    = ");
    fd = ufs_open(resolved, UFS_O_RDONLY);
    if (fd < 0) {
        print_error("show open");
        return;
    }
    for (;;) {
        ssize_t amount = ufs_read(fd, buffer, sizeof(buffer));
        if (amount < 0) {
            print_error("show read");
            (void)ufs_close(fd);
            return;
        }
        if (amount == 0) {
            break;
        }
        (void)fwrite(buffer, 1, (size_t)amount, stdout);
    }
    printf("\n");
    if (ufs_close(fd) != 0) {
        print_error("show close");
    }
}

static void command_super(void)
{
    if (!require_shell_mount()) {
        return;
    }
    printf("magic:              0x%08x\n", (unsigned int)g_ufs.sb.magic);
    printf("version:            %u\n", (unsigned int)g_ufs.sb.version);
    printf("block size:         %u bytes\n",
           (unsigned int)g_ufs.sb.block_size);
    printf("total blocks:       %u\n", (unsigned int)g_ufs.sb.total_blocks);
    printf("free data blocks:   %u\n", (unsigned int)g_ufs.sb.free_blocks);
    printf("total inodes:       %u\n", (unsigned int)g_ufs.sb.total_inodes);
    printf("free inodes:        %u\n", (unsigned int)g_ufs.sb.free_inodes);
    printf("inode table blocks: %u-%u\n",
           (unsigned int)g_ufs.sb.inode_table_start,
           (unsigned int)(g_ufs.sb.inode_table_start +
                          g_ufs.sb.inode_table_blocks - 1U));
    printf("data region blocks: %u-%u\n",
           (unsigned int)g_ufs.sb.data_region_start,
           (unsigned int)(g_ufs.sb.data_region_start +
                          g_ufs.sb.data_region_blocks - 1U));
}

static void command_bitmap(void)
{
    uint8_t inode_bitmap[UFS_BLOCK_SIZE];
    uint8_t block_bitmap[UFS_BLOCK_SIZE];
    uint32_t index;
    uint32_t allocated_data = 0;

    if (!require_shell_mount()) {
        return;
    }
    if (read_inode_bitmap(inode_bitmap) != 0 ||
        read_block_bitmap(block_bitmap) != 0) {
        print_error("read bitmap");
        return;
    }

    printf("Allocated inodes: ");
    for (index = 0; index < UFS_MAX_INODES; ++index) {
        if (bitmap_test(inode_bitmap, index)) {
            printf("%u ", (unsigned int)index);
        }
    }
    printf("\nAllocated data/pointer blocks: ");
    for (index = UFS_DATA_REGION_START_BLK; index < UFS_TOTAL_BLOCKS; ++index) {
        if (bitmap_test(block_bitmap, index)) {
            ++allocated_data;
            if (allocated_data <= 80U) {
                printf("%u ", (unsigned int)index);
            }
        }
    }
    if (allocated_data > 80U) {
        printf("... ");
    }
    printf("\nAllocated data/pointer count: %u\n",
           (unsigned int)allocated_data);
}

static int execute_line(char *line)
{
    char *command = strtok(line, " \t\r\n");
    char *path;

    if (command == NULL) {
        return 1;
    }
    if (strcmp(command, "help") == 0) {
        show_help();
    } else if (strcmp(command, "format") == 0) {
        command_format();
    } else if (strcmp(command, "mount") == 0) {
        command_mount();
    } else if (strcmp(command, "unmount") == 0) {
        command_unmount();
    } else if (strcmp(command, "pwd") == 0) {
        command_pwd();
    } else if (strcmp(command, "cd") == 0) {
        command_cd(strtok(NULL, " \t\r\n"));
    } else if (strcmp(command, "mkdir") == 0 ||
               strcmp(command, "create") == 0 ||
               strcmp(command, "touch") == 0) {
        path = strtok(NULL, " \t\r\n");
        command_create(command, path);
    } else if (strcmp(command, "rmdir") == 0 || strcmp(command, "rm") == 0) {
        path = strtok(NULL, " \t\r\n");
        command_remove(command, path);
    } else if (strcmp(command, "write") == 0 ||
               strcmp(command, "append") == 0) {
        char *text;
        path = strtok(NULL, " \t\r\n");
        text = skip_spaces(strtok(NULL, "\r\n"));
        command_write(path, text, strcmp(command, "append") == 0);
    } else if (strcmp(command, "cat") == 0) {
        command_cat(strtok(NULL, " \t\r\n"));
    } else if (strcmp(command, "ls") == 0) {
        command_ls(strtok(NULL, " \t\r\n"));
    } else if (strcmp(command, "stat") == 0) {
        command_stat(strtok(NULL, " \t\r\n"));
    } else if (strcmp(command, "truncate") == 0) {
        path = strtok(NULL, " \t\r\n");
        command_truncate(path, strtok(NULL, " \t\r\n"));
    } else if (strcmp(command, "inode") == 0) {
        command_inode(strtok(NULL, " \t\r\n"));
    } else if (strcmp(command, "map") == 0) {
        command_map(strtok(NULL, " \t\r\n"));
    } else if (strcmp(command, "show") == 0) {
        command_show(strtok(NULL, " \t\r\n"));
    } else if (strcmp(command, "bitmap") == 0) {
        command_bitmap();
    } else if (strcmp(command, "super") == 0) {
        command_super();
    } else if (strcmp(command, "quit") == 0 || strcmp(command, "exit") == 0) {
        return 0;
    } else {
        printf("Unknown command '%s'. Run 'help'.\n", command);
    }
    return 1;
}

int main(int argc, char **argv)
{
    char line[INPUT_SIZE];

    image_path = argc > 1 ? argv[1] : DEFAULT_IMAGE;
    printf("UserFS interactive demonstration\n");
    printf("Image: %s\n", image_path);
    printf("Run 'help' to see commands.\n\n");

    for (;;) {
        if (mounted) {
            printf("userfs:%s$ ", current_directory);
        } else {
            printf("userfs(unmounted)> ");
        }
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\n");
            break;
        }
        if (!execute_line(line)) {
            break;
        }
    }

    if (mounted) {
        if (ufs_unmount() != 0) {
            print_error("automatic unmount");
            return EXIT_FAILURE;
        }
        printf("Unmounted automatically before exit.\n");
    }
    return EXIT_SUCCESS;
}
