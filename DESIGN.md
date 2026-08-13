# User Filesystem Design

## 1. Architecture

The User Filesystem (UFS) is a small filesystem implemented on top of a fixed-size Linux disk image.

The filesystem uses a layered design:

1. **Public API layer**
   - Provides filesystem operations such as:
     - `ufs_format()`
     - `ufs_mount()`
     - `ufs_unmount()`
     - `ufs_mkdir()`
     - `ufs_create()`
     - `ufs_open()`
     - `ufs_read()`
     - `ufs_write()`
     - `ufs_seek()`
     - `ufs_truncate()`
     - `ufs_stat()`

2. **Filesystem lifecycle and state**
   - Maintains the currently mounted filesystem.
   - Stores the Linux image file descriptor.
   - Keeps an in-memory copy of the superblock.
   - Maintains the open-file descriptor table.
   - Only one filesystem image can be mounted at a time.

3. **Storage layer**
   - Accesses the disk image using Linux file operations.
   - Uses exact read and write helpers to guarantee that the requested number of bytes is transferred.
   - Provides block-level access using the fixed filesystem block size of 512 bytes.

4. **Metadata layer**
   - The superblock describes the filesystem layout and free-space information.
   - The inode bitmap tracks allocated and free inodes.
   - The block bitmap tracks allocated and free blocks.
   - The inode table stores file and directory metadata.

5. **Namespace and file I/O**
   - Directory and path operations resolve filesystem paths and manage directory entries.
   - File I/O operations use file descriptors containing an inode number, current offset, and open flags.

---

## 2. Disk Layout

The filesystem uses a fixed-size 1 MiB disk image.

Each filesystem block has a size of 512 bytes.

The filesystem contains 2048 blocks:

```text
2048 × 512 = 1,048,576 bytes = 1 MiB



