# UserFS Manual

## Overview

UserFS is a small filesystem implemented as a C library. It operates entirely in user space and stores all persistent state within a single regular Linux file (typically a 1 MiB disk image). It provides a familiar POSIX-like API without requiring FUSE or modifying the host kernel. 

This manual covers both the C API for integrating UserFS into your own applications and the Interactive Presentation Shell for operating it from the command line.

---

## 1. C Library API (`userfs.h`)

To use UserFS in your programs, include `userfs.h`. All functions return `0` or a positive number on success, and `-1` on failure (setting standard `errno`).

### Core Operations

*   `int ufs_format(const char *image_path, size_t image_size)`
    Formats a new filesystem image at the given path with the specified size. For the current implementation, the image size should be fixed to 1 MiB (`1024 * 1024` bytes).
*   `int ufs_mount(const char *image_path)`
    Mounts an existing formatted filesystem image.
*   `int ufs_unmount(void)`
    Flushes all changes to disk and unmounts the current image.

### Directory Operations

*   `int ufs_mkdir(const char *path)`
    Creates a new directory.
*   `int ufs_rmdir(const char *path)`
    Removes an empty directory.
*   `int ufs_listdir(const char *path, struct ufs_dirent *entries, size_t max_entries)`
    Populates the `entries` array with the contents of a directory.

### File Operations

*   `int ufs_create(const char *path)`
    Creates an empty file at the given path (similar to `touch`).
*   `int ufs_unlink(const char *path)`
    Deletes a file.
*   `int ufs_open(const char *path, int flags)`
    Opens a file and returns a file descriptor. Flags can be `UFS_O_RDONLY`, `UFS_O_WRONLY`, `UFS_O_RDWR`, or `UFS_O_APPEND`.
*   `int ufs_close(int fd)`
    Closes an open file descriptor.
*   `ssize_t ufs_read(int fd, void *buf, size_t count)`
    Reads `count` bytes from the file descriptor into `buf`. Returns the number of bytes read.
*   `ssize_t ufs_write(int fd, const void *buf, size_t count)`
    Writes `count` bytes from `buf` to the file descriptor. Returns the number of bytes written.
*   `off_t ufs_seek(int fd, off_t offset, int whence)`
    Moves the read/write pointer of the file descriptor.
*   `int ufs_truncate(const char *path, size_t size)`
    Resizes a file to exactly `size` bytes. Truncating beyond the current end-of-file zero-fills the gap.
*   `int ufs_stat(const char *path, struct ufs_stat *st)`
    Retrieves metadata about a file or directory, such as its type (`UFS_TYPE_FILE` or `UFS_TYPE_DIR`) and size.

---

## 2. Interactive Presentation Shell (`userfs_shell`)

The UserFS shell provides a virtual command-line interface to interact directly with a UserFS image. It maintains its own virtual working directory and never modifies the host filesystem.

### Standard Commands

*   `format` - Formats the disk image.
*   `mount` - Mounts the disk image for interaction.
*   `unmount` - Unmounts and flushes the filesystem.
*   `cd <path>` - Changes the virtual working directory. Supports absolute paths, relative paths, `.`, and `..`.
*   `pwd` - Prints the current virtual working directory.
*   `ls [<path>]` - Lists the contents of the specified directory (or the current directory if omitted).
*   `mkdir <path>` - Creates a new directory.
*   `rmdir <path>` - Removes an empty directory.
*   `create <path>` (or `touch <path>`) - Creates a new empty file.
*   `rm <path>` - Deletes a file.
*   `write <path> <text...>` - Opens a file, truncates it to 0, writes the given text to it, and closes it.
*   `append <path> <text...>` - Opens a file in append mode, writes the text to the end, and closes it.
*   `cat <path>` - Reads and prints the entire contents of a file.
*   `truncate <path> <size>` - Truncates or extends a file to the specified size.
*   `stat <path>` - Displays file/directory metadata (type and size).
*   `quit` (or `exit`) - Exits the shell.

### Debug / Inspection Commands

These commands break the standard API abstraction to expose the raw physical structures on disk. They are useful for understanding the inner workings of UserFS.

*   `super` - Displays the superblock data (total blocks, metadata region limits).
*   `bitmap` - Displays the current state of the inode and block allocation bitmaps.
*   `inode <path>` - Dumps the raw inode structure for a file (size, direct block pointers, etc).
*   `map <path>` - Prints the logical-to-physical block mapping for a file, showing exactly which physical disk blocks store which logical file blocks.
*   `show <path>` - A unified inspector that combines `stat`, `inode`, `map`, `bitmap`, and `cat` into one comprehensive view for a single file.

---

## Technical Limitations

*   **Size Limits:** 1 MiB fixed disk image, 512-byte blocks.
*   **Capacity:** 256 inodes maximum.
*   **Open Files:** Up to 32 simultaneously open file descriptors.
*   **Names:** File/directory names up to 31 characters; full paths up to 255 characters.
*   **Directories:** Max 64 entries per directory.
*   **Features:** No permissions, hard/symlinks, timestamps, journaling, or concurrent processes.
