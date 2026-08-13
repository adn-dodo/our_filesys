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
     - `ufs_rmdir()`
     - `ufs_listdir()`
     - `ufs_create()`
     - `ufs_unlink()`
     - `ufs_open()`
     - `ufs_close()`
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

The disk image is divided into the following regions:

Blocks	Region	Description
0	Superblock	Filesystem metadata and layout information
1	Inode bitmap	Tracks allocated and free inodes
2	Block bitmap	Tracks allocated and free blocks
3–34	Inode table	Stores 256 inodes
35–2047	Data region	Stores file and directory data
Superblock

Block 0 contains the filesystem superblock.

It stores:

Filesystem magic number
Filesystem version
Block size
Total number of blocks
Inode bitmap location
Block bitmap location
Inode table location
Data region location
Total number of inodes
Root inode number
Number of free inodes
Number of free data blocks
Inode Bitmap

Block 1 contains the inode bitmap.

There are 256 inodes.

At format time:

Inode 0 = allocated
Inodes 1–255 = free

Therefore:

free_inodes = 255
Block Bitmap

Block 2 contains the block bitmap.

Metadata blocks 0–34 are permanently marked as occupied.

The data region starts at block 35.

Therefore the number of available data blocks is:

2048 - 35 = 2013

At format time:

free_blocks = 2013
Inode Table

The inode table occupies blocks 3–34.

That gives:

32 blocks × 512 bytes = 16384 bytes

Each inode is 64 bytes:

16384 / 64 = 256 inodes

Inode 0 is the root directory inode.

Data Region

The data region occupies blocks 35–2047.

These blocks are allocated dynamically when files or directories require storage.

Newly allocated data blocks are zero-filled before use.

3. Filesystem Structures
Superblock

The ufs_superblock structure is exactly 512 bytes.

It describes the filesystem identity, layout, and free-space information.

Inode

Each ufs_inode is exactly 64 bytes.

An inode stores:

File or directory type
Flags
File size
Number of allocated data blocks
Eight direct block pointers
One single-indirect pointer
One double-indirect pointer

Unused block pointers are initialized to UFS_INVALID_BLOCK.

Directory Entry

Each on-disk directory entry is exactly 64 bytes.

A directory entry stores:

Whether the entry is currently used
The inode number
The object type
The filename
File Descriptor

File descriptors are maintained only in memory.

Each descriptor stores:

Whether the descriptor is active
The inode number
Current file offset
Open flags

The filesystem supports up to 32 simultaneously open files.

4. Filesystem Lifecycle

The filesystem lifecycle consists of three main operations:

ufs_format()
      ↓
ufs_mount()
      ↓
filesystem operations
      ↓
ufs_unmount()
Format

ufs_format() creates a new filesystem image.

It:

Validates the image path.
Checks that the requested image size is exactly 1 MiB.
Creates or overwrites the image.
Sets its size to 1 MiB.
Clears the image.
Creates the superblock.
Initializes the inode bitmap.
Initializes the block bitmap.
Creates inode 0 as the root directory.
Writes the filesystem metadata.
Flushes the image using fsync().
Closes the image.

After formatting:

root inode = 0
free inodes = 255
free data blocks = 2013
Mount

ufs_mount() opens an existing filesystem image.

It:

Rejects mounting if another filesystem is already mounted.
Opens the image using O_RDWR.
Reads the superblock.
Validates the magic number.
Validates the filesystem version.
Validates the block size.
Validates the total number of blocks.
Validates the expected filesystem layout.
Stores the superblock in memory.
Resets the descriptor table.
Marks the filesystem as mounted.

A corrupted or invalid image is rejected.

Unmount

ufs_unmount():

Rejects the operation if no filesystem is mounted.
Flushes pending changes using fsync().
Closes the Linux image descriptor.
Clears the mounted state.
Clears the in-memory superblock.
Resets the descriptor table.

The filesystem can then be mounted again using the same image.

5. Storage I/O

The filesystem uses exact Linux image I/O helpers.

Exact Read

The exact-read helper uses pread() repeatedly until the requested number of bytes has been read.

This prevents partial reads from being treated as successful operations.

Exact Write

The exact-write helper uses pwrite() repeatedly until the requested number of bytes has been written.

This prevents partial writes from leaving incomplete filesystem blocks.

Block I/O

The filesystem provides block-level helpers:

read_block()
write_block()

Each operation transfers exactly one filesystem block:

512 bytes

Block numbers are checked to ensure that they are within the valid range:

0–2047
6. Allocation

The filesystem uses persistent bitmaps to track free resources.

Inode Allocation

The inode bitmap records which inodes are currently allocated.

When a new file or directory is created:

Find a free inode.
Mark the inode as allocated.
Initialize its metadata.
Decrease free_inodes.

When an inode is released:

Release its data blocks.
Clear its inode bitmap bit.
Increase free_inodes.
Data Block Allocation

Only blocks in the data region can be allocated dynamically.

The valid data blocks are:

35–2047

Metadata blocks 0–34 can never be allocated as file data.

When a data block is allocated:

Find a free data block.
Mark it as occupied.
Zero the block.
Decrease free_blocks.

When a data block is released:

Clear its bitmap bit.
Increase free_blocks.
7. Inode Block Mapping

Each inode supports:

8 direct pointers
1 single-indirect pointer
1 double-indirect pointer

The direct pointers reference data blocks directly.

The single-indirect pointer references a block containing additional data-block addresses.

The double-indirect pointer references a block of pointers to additional pointer blocks.

This allows files to grow beyond the capacity provided by the eight direct pointers.

Indirect blocks are allocated only when they are required.

Unused indirect pointers are initialized to UFS_INVALID_BLOCK.

8. Paths and Directories

Filesystem paths are absolute paths beginning with /.

For example:

/docs/course/notes.txt

Path resolution is divided into two operations.

resolve_path()

resolve_path() finds the inode belonging to an existing path.

resolve_parent()

resolve_parent() finds the parent directory and extracts the final component.

For:

/docs/course/notes.txt

the result is:

Parent: /docs/course
Name: notes.txt

Directory operations include:

directory_find()
directory_add()
directory_remove()
directory_is_empty()

The filesystem prevents duplicate directory names.

Invalid or missing paths return appropriate errors such as:

EINVAL
ENOENT
ENOTDIR
EEXIST
EISDIR
ENOTEMPTY

The root directory / cannot be removed.

A directory cannot be unlinked using ufs_unlink().

A regular file cannot be removed using ufs_rmdir().

A non-empty directory cannot be removed.

9. File Descriptors and File I/O

The filesystem maintains an in-memory table of 32 file descriptors.

Each descriptor contains:

in_use
inode_number
offset
flags
Open

ufs_open():

Resolves the path.
Verifies that the object is a regular file.
Validates the requested flags.
Allocates a free descriptor.
Initializes the offset to zero.
Returns the descriptor number.

Opening a directory as a regular file is rejected.

The descriptor table supports a maximum of 32 simultaneously open files.

Close

ufs_close() validates the descriptor and releases its slot for reuse.

Invalid or already closed descriptors are rejected.

Read

ufs_read():

Starts at the current descriptor offset.
Reads data from the file.
Handles block boundaries.
Stops at EOF.
Returns zero when the current offset is at EOF.
Advances the descriptor offset.

Read operations are rejected for write-only descriptors.

Write

ufs_write():

Writes starting at the current offset.
Allocates data blocks when necessary.
Handles writes across block boundaries.
Updates the inode size.
Advances the descriptor offset.
Supports append mode.
Zero-fills gaps when writing beyond the current EOF.

Write operations are rejected for read-only descriptors.

If there is not enough free space, ufs_write() returns ENOSPC without leaking blocks.

Seek

ufs_seek() supports:

SEEK_SET
SEEK_CUR
SEEK_END

The resulting offset cannot be negative.

Seeking beyond EOF is allowed.

No data blocks are allocated by ufs_seek() itself.

10. Truncation

ufs_truncate() changes the size of a regular file.

Growing

When a file is grown:

Additional blocks are allocated when required.
The newly created file range is zero-filled.
The file size is updated.
Shrinking

When a file is shrunk:

Unnecessary data blocks are released.
Unused indirect blocks are released.
The file size is updated.
Truncate to Zero

Truncating a file to zero releases all allocated data blocks and leaves the file with size zero.

If there is not enough space for a requested growth operation, the filesystem returns:

ENOSPC

The implementation must avoid leaking blocks when an allocation fails.

11. Persistence

Filesystem metadata is stored inside the disk image.

Therefore, important information survives unmounting and remounting.

Persistent information includes:

Superblock information
Free inode count
Free block count
Inode bitmap
Block bitmap
Inode contents
Directory entries
File contents

The expected lifecycle is:

format
  ↓
mount
  ↓
create/write files
  ↓
unmount
  ↓
mount again
  ↓
verify files and metadata

The root directory must still exist after remounting.

12. Error Handling

Filesystem operations return -1 on failure and set errno to describe the error.

Important errors include:

Error	Meaning
EINVAL	Invalid argument or invalid path
ENOENT	Requested object does not exist
EEXIST	Object already exists
ENOTDIR	A path component is not a directory
EISDIR	Operation is invalid for a directory
ENOTEMPTY	Directory is not empty
ENOSPC	No free inode or data block
EBUSY	Filesystem is already mounted
EBADF	Invalid or closed file descriptor
13. Integration

The project is divided among multiple members.

Member 1: Architecture, lifecycle, formatting, mounting and unmounting.
Member 2: Bitmaps, inode management, block allocation and truncation.
Member 3: Paths, directories and namespace operations.
Member 4: File descriptors, reading, writing and seeking.
Member 5: Testing, documentation and demonstration.

All members' implementations are integrated into the same filesystem.

Compilation should be performed after every integration step to detect interface and structure conflicts early.

14. Acceptance Tests

The main lifecycle acceptance test is:

Format a 1 MiB image
        ↓
Mount
        ↓
stat("/")
        ↓
Unmount
        ↓
Mount the same image again
        ↓
stat("/")
        ↓
Unmount

Expected result:

The root directory exists before and after remounting.

The complete test suite should additionally verify:

Format, mount and unmount
Root directory operations
Nested directories
File creation and deletion
Directory listing
File statistics
Reading and writing
Block-boundary operations
Seeking
Append mode
Truncation
Invalid descriptors
Maximum 32 descriptors
Inode exhaustion
Data-block exhaustion
Persistence after remount
Detection of corrupted filesystem images



