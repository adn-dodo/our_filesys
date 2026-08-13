# User Filesystem Design

## 1. Architecture

The User Filesystem (UFS) is a small filesystem implemented on top of a fixed-size Linux disk image.

The filesystem is organized into several layers. Each layer has a specific responsibility and communicates with the layers below it.

### 1.1 Public API Layer

The public API provides the operations used by applications and test programs.

The main operations are:

- `ufs_format()`
- `ufs_mount()`
- `ufs_unmount()`
- `ufs_mkdir()`
- `ufs_rmdir()`
- `ufs_listdir()`
- `ufs_create()`
- `ufs_unlink()`
- `ufs_stat()`
- `ufs_open()`
- `ufs_close()`
- `ufs_read()`
- `ufs_write()`
- `ufs_seek()`
- `ufs_truncate()`

Applications interact with the filesystem only through these public functions.

---

### 1.2 Filesystem Lifecycle and Mounted State

The filesystem maintains one global mounted-filesystem state.

The mounted state contains:

- The Linux file descriptor of the disk image.
- An in-memory copy of the superblock.
- A flag indicating whether a filesystem is currently mounted.
- The open-file descriptor table.

Only one filesystem image can be mounted at a time.

The lifecycle is:

```text
ufs_format()
     |
     v
  Disk Image
     |
     v
ufs_mount()
     |
     v
Mounted Filesystem
     |
     +------------------+
     |                  |
     v                  v
Filesystem Operations   Open Descriptors
     |                  |
     +--------+---------+
              |
              v
        ufs_unmount()
              |
              v
        Closed Image
```

---

### 1.3 Storage Layer

The storage layer provides low-level access to the Linux disk image.

It uses Linux system calls such as:

- `open()`
- `close()`
- `pread()`
- `pwrite()`
- `fsync()`
- `ftruncate()`

The storage layer provides exact-read and exact-write helpers.

These helpers ensure that the requested number of bytes is transferred completely.

Block-level helpers are also provided:

- Read one 512-byte block.
- Write one 512-byte block.
- Zero one block.

The filesystem uses a fixed block size of **512 bytes**.

---

### 1.4 Metadata Layer

The metadata layer manages filesystem information.

It contains:

- The superblock.
- The inode bitmap.
- The block bitmap.
- The inode table.
- Inodes.
- Data-block allocation information.

The superblock describes the filesystem layout and stores free-space counters.

The inode bitmap records which inodes are allocated.

The block bitmap records which filesystem blocks are allocated.

The inode table stores metadata for all filesystem objects.

---

### 1.5 Namespace Layer

The namespace layer manages directories and paths.

It is responsible for:

- Validating paths.
- Resolving paths to inode numbers.
- Resolving parent directories.
- Searching directory entries.
- Adding directory entries.
- Removing directory entries.
- Creating directories.
- Removing directories.
- Creating files.
- Removing files.
- Listing directories.

Paths are resolved by starting at the root inode and traversing one component at a time.

---

### 1.6 File I/O Layer

The file I/O layer provides descriptor-based access to regular files.

Each open descriptor stores:

- The inode number.
- The current file offset.
- The open flags.
- Whether the descriptor slot is active.

The filesystem supports:

- `read()`
- `write()`
- `seek()`
- Append mode.
- Read-only access.
- Write-only access.
- Read/write access.

The descriptor table contains **32 entries**.

---

# 2. Disk Layout

The filesystem uses a fixed-size **1 MiB disk image**.

Each filesystem block has a size of **512 bytes**.

The image contains **2048 blocks**.

```text
2048 × 512 = 1,048,576 bytes = 1 MiB
```

The complete disk layout is:

| Blocks | Region | Description |
|---|---|---|
| 0 | Superblock | Filesystem metadata and layout information |
| 1 | Inode bitmap | Tracks allocated and free inodes |
| 2 | Block bitmap | Tracks allocated and free blocks |
| 3–34 | Inode table | Stores 256 inodes |
| 35–2047 | Data region | Stores file and directory data |

The layout is fixed and must not change between formatting and mounting.

---

## 2.1 Superblock

Block `0` contains the filesystem superblock.

The superblock stores:

- Magic number.
- Filesystem version.
- Block size.
- Total number of blocks.
- Inode bitmap location.
- Block bitmap location.
- Inode table location.
- Data region location.
- Total number of inodes.
- Root inode number.
- Number of free inodes.
- Number of free data blocks.

The superblock occupies exactly one 512-byte block.

---

## 2.2 Inode Bitmap

Block `1` contains the inode bitmap.

There are 256 inodes.

Each bit represents one inode:

```text
0 = free
1 = allocated
```

Initially:

```text
inode 0 = allocated
inode 1..255 = free
```

Therefore:

```text
free_inodes = 255
```

---

## 2.3 Block Bitmap

Block `2` contains the block bitmap.

Every filesystem block has one corresponding allocation bit.

Blocks `0–34` are metadata blocks and must always be marked as occupied.

The data region contains:

```text
2048 - 35 = 2013 blocks
```

Therefore, immediately after formatting:

```text
free_blocks = 2013
```

No metadata block may ever be returned by the data-block allocator.

---

## 2.4 Inode Table

The inode table occupies blocks `3–34`.

That is:

```text
32 blocks × 512 bytes = 16,384 bytes
```

Each inode is exactly 64 bytes:

```text
16,384 / 64 = 256 inodes
```

Therefore the inode table contains exactly:

```text
256 inodes
```

Inode `0` is reserved for the root directory.

---

## 2.5 Data Region

The data region starts at block `35` and continues through block `2047`.

```text
Data blocks = 2048 - 35 = 2013
```

These blocks store:

- Regular file contents.
- Directory entries.
- Indirect pointer blocks.

Blocks in the data region are allocated only when needed.

---

# 3. Filesystem Structures

The filesystem defines several fixed-size on-disk structures.

## 3.1 Superblock Structure

The superblock is exactly 512 bytes.

```c
struct ufs_superblock {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;

    uint32_t inode_bitmap_start;
    uint32_t inode_bitmap_blocks;

    uint32_t block_bitmap_start;
    uint32_t block_bitmap_blocks;

    uint32_t inode_table_start;
    uint32_t inode_table_blocks;

    uint32_t data_region_start;
    uint32_t data_region_blocks;

    uint32_t total_inodes;
    uint32_t root_inode;

    uint32_t free_inodes;
    uint32_t free_blocks;

    uint8_t padding[448];
};
```

---

## 3.2 Inode Structure

Each inode is exactly 64 bytes.

An inode stores:

- Object type.
- Flags.
- File size.
- Number of allocated data blocks.
- Eight direct block pointers.
- One single-indirect pointer.
- One double-indirect pointer.

```c
struct ufs_inode {
    uint32_t type;
    uint32_t flags;

    uint64_t size;
    uint32_t block_count;

    uint32_t direct[8];

    uint32_t single_indirect;
    uint32_t double_indirect;

    uint32_t reserved;
};
```

Unused block pointers contain:

```c
UFS_INVALID_BLOCK
```

---

## 3.3 Directory Entry

Each directory entry is exactly 64 bytes.

A directory entry contains:

- Whether the entry is used.
- The inode number.
- The object type.
- The file or directory name.

```c
struct ufs_disk_dirent {
    uint32_t used;
    uint32_t inode_number;
    uint32_t type;

    char name[UFS_MAX_NAME + 1];

    uint32_t reserved[5];
};
```

---

# 4. Filesystem Lifecycle

The filesystem lifecycle consists of:

```text
Format → Mount → Use → Unmount
```

The same image must be able to survive an unmount and later be mounted again.

---

## 4.1 Formatting

`ufs_format()` creates a new valid filesystem image.

The function must:

1. Validate the image path.
2. Create or overwrite the image.
3. Set the image size to exactly 1 MiB.
4. Zero the filesystem metadata.
5. Create the superblock.
6. Initialize the inode bitmap.
7. Initialize the block bitmap.
8. Mark blocks `0–34` as occupied.
9. Mark inode `0` as occupied.
10. Initialize inode `0` as the root directory.
11. Set `free_inodes` to `255`.
12. Set `free_blocks` to `2013`.
13. Write all required metadata to the image.

After formatting, the filesystem is ready to be mounted.

---

## 4.2 Root Directory

Inode `0` is always the root inode.

The root inode represents `/`.

After formatting:

```text
Root inode = 0
Root type  = directory
Root size  = 0
```

The root directory does not need to allocate a data block until directory entries are required.

The root inode must remain allocated for the entire lifetime of the filesystem.

---

## 4.3 Mounting

`ufs_mount()` opens an existing filesystem image and validates it.

The function must:

1. Reject the operation if another filesystem is already mounted.
2. Open the image.
3. Read the superblock.
4. Validate the magic number.
5. Validate the filesystem version.
6. Validate the block size.
7. Validate the total number of blocks.
8. Validate the bitmap locations.
9. Validate the inode table location.
10. Validate the data region.
11. Store the superblock in memory.
12. Reset the descriptor table.
13. Mark the filesystem as mounted.

A corrupted or incompatible image must be rejected.

---

## 4.4 Unmounting

`ufs_unmount()` closes the currently mounted filesystem.

The function must:

1. Reject the operation if no filesystem is mounted.
2. Flush pending changes using `fsync()`.
3. Close the Linux image descriptor.
4. Clear the mounted state.
5. Reset the descriptor table.

After successful unmounting, filesystem operations requiring a mounted filesystem must fail.

---

# 5. Inode and Block Allocation

## 5.1 Bitmap Operations

The filesystem uses bitmaps to track allocated resources.

The basic bitmap operations are:

```text
test_bit()
set_bit()
clear_bit()
```

The same operations are used for both the inode bitmap and the block bitmap.

---

## 5.2 Inode Allocation

When a new filesystem object is created, the filesystem searches for a free inode.

The allocation procedure is:

```text
Find free inode
      |
      v
Set inode bitmap bit
      |
      v
Initialize inode
      |
      v
Decrease free_inodes
      |
      v
Write inode
```

When an inode is released:

```text
Release inode
      |
      v
Clear inode bitmap bit
      |
      v
Decrease allocated count
      |
      v
Increase free_inodes
```

Inode `0` must never be released.

---

## 5.3 Data-Block Allocation

Data blocks are allocated only from:

```text
35–2047
```

The allocation procedure is:

```text
Search data region
      |
      v
Find free block
      |
      v
Set block bitmap bit
      |
      v
Zero the block
      |
      v
Decrease free_blocks
```

Every newly allocated block must be zero-filled.

This prevents old data from becoming visible to a newly created file.

---

## 5.4 Data-Block Release

When a block is no longer needed:

1. Verify that it belongs to the data region.
2. Clear its bitmap bit.
3. Increase `free_blocks`.
4. Remove the corresponding inode pointer if necessary.

Released blocks must become available for future allocation.

---

# 6. Inode Block Mapping

Each inode supports:

```text
8 direct pointers
1 single-indirect pointer
1 double-indirect pointer
```

The mapping is:

```text
Logical file block
        |
        +---- 0..7
        |      |
        |      +--> Direct block
        |
        +---- After direct blocks
        |      |
        |      +--> Single indirect
        |
        +---- Larger files
               |
               +--> Double indirect
```

---

## 6.1 Direct Blocks

Logical blocks `0–7` are mapped directly using the inode's eight direct pointers.

For example:

```text
logical block 0 → direct[0]
logical block 1 → direct[1]
...
logical block 7 → direct[7]
```

---

## 6.2 Single-Indirect Block

After the eight direct blocks, the inode uses the single-indirect pointer.

The single-indirect block contains an array of physical block numbers.

The pointer block itself is allocated only when required.

---

## 6.3 Double-Indirect Block

For larger files, the double-indirect pointer is used.

The double-indirect structure is:

```text
Inode
  |
  v
Double-indirect block
  |
  +--> Indirect block
  |       |
  |       +--> Data block
  |
  +--> Indirect block
          |
          +--> Data block
```

Indirect blocks are allocated only when required.

---

# 7. Truncation

`ufs_truncate()` changes the size of a file.

Two main cases are supported:

```text
Growing a file
Shrinking a file
```

---

## 7.1 Growing a File

When the new size is larger than the current size:

```text
old size < new size
```

The filesystem must:

1. Allocate the required blocks.
2. Zero-fill newly created ranges.
3. Update the inode size.

If the file grows into a partially used block, the newly exposed bytes must be zero.

---

## 7.2 Shrinking a File

When the new size is smaller:

```text
new size < old size
```

The filesystem must:

1. Update the file size.
2. Release blocks that are no longer required.
3. Release unused indirect blocks.
4. Update the block bitmap.
5. Update `free_blocks`.

---

## 7.3 Truncating to Zero

Truncating a file to zero must release all data blocks associated with the file.

The inode remains allocated, but:

```text
size = 0
block_count = 0
```

All unused indirect structures must also be released.

---

## 7.4 ENOSPC Safety

If a grow operation cannot allocate enough blocks:

```text
errno = ENOSPC
```

The operation must not leave permanently leaked blocks.

The filesystem must maintain consistent bitmap and inode information.

---

# 8. Paths and Namespace

## 8.1 Path Validation

The filesystem accepts absolute paths beginning with `/`.

Examples of valid paths:

```text
/
/docs
/docs/course
/docs/course/notes.txt
```

Paths longer than `UFS_MAX_PATH` are rejected.

Path components longer than `UFS_MAX_NAME` are rejected.

Invalid paths return:

```text
EINVAL
```

---

## 8.2 Root Path

The path:

```text
/
```

always refers to inode `0`.

Resolving `/` must return the root inode.

The root directory cannot be deleted.

---

## 8.3 Repeated `/`

Repeated separators are treated as separators between components.

For example:

```text
/docs//course/notes.txt
```

may be normalized to:

```text
/docs/course/notes.txt
```

The behavior must remain consistent throughout path resolution.

---

## 8.4 Path Resolution

`resolve_path()` searches for an existing filesystem object.

For:

```text
/docs/course/notes.txt
```

the resolver performs:

```text
root inode
    |
    v
"docs"
    |
    v
"course"
    |
    v
"notes.txt"
```

If any component does not exist:

```text
ENOENT
```

is returned.

If traversal attempts to continue through a regular file:

```text
ENOTDIR
```

is returned.

---

## 8.5 Parent Resolution

`resolve_parent()` resolves the parent directory and extracts the final component.

For:

```text
/docs/course/notes.txt
```

the result is:

```text
Parent: /docs/course
Name:   notes.txt
```

This helper is used by creation and deletion operations.

---

# 9. Directory Management

## 9.1 Directory Entries

A directory contains an array of directory entries.

Each entry maps:

```text
name → inode number
```

Example:

```text
/docs
```

may contain:

```text
course → inode 2
```

---

## 9.2 Finding an Entry

`directory_find()` searches a directory for a given name.

If the name exists, the corresponding inode number is returned.

If the name does not exist:

```text
ENOENT
```

is returned.

---

## 9.3 Adding an Entry

`directory_add()` adds a new directory entry.

Before adding:

1. Validate the name.
2. Check whether the name already exists.
3. Find a free directory-entry slot.
4. Allocate a directory block if necessary.
5. Store the inode number and type.

Duplicate names are rejected with:

```text
EEXIST
```

---

## 9.4 Removing an Entry

`directory_remove()` removes a directory entry from its parent directory.

The inode itself is released separately after the namespace entry has been removed.

---

## 9.5 Empty Directory Check

`directory_is_empty()` checks whether a directory contains any active entries.

A non-empty directory cannot be removed.

The expected error is:

```text
ENOTEMPTY
```

---

# 10. Public Directory Operations

## 10.1 `ufs_mkdir()`

`ufs_mkdir()` creates a new directory.

The operation is:

```text
Validate path
     |
     v
Resolve parent
     |
     v
Check duplicate name
     |
     v
Allocate inode
     |
     v
Initialize directory inode
     |
     v
Add directory entry
```

If the target already exists:

```text
EEXIST
```

---

## 10.2 `ufs_rmdir()`

`ufs_rmdir()` removes an empty directory.

It must reject:

- The root directory.
- Regular files.
- Non-empty directories.
- Missing paths.

Expected errors include:

```text
EISDIR
ENOTEMPTY
ENOENT
EINVAL
```

---

## 10.3 `ufs_listdir()`

`ufs_listdir()` lists the entries contained in a directory.

The function must:

1. Resolve the path.
2. Verify that the inode is a directory.
3. Read directory entries.
4. Return the active entries.

Attempting to list a regular file must return:

```text
ENOTDIR
```

---

# 11. File Creation and Deletion

## 11.1 `ufs_create()`

Creating a file requires:

1. Validating the path.
2. Resolving the parent directory.
3. Checking for duplicate names.
4. Allocating a free inode.
5. Initializing the inode as a regular file.
6. Adding a directory entry.

A duplicate name returns:

```text
EEXIST
```

---

## 11.2 `ufs_unlink()`

`ufs_unlink()` removes a regular file.

The operation must:

1. Resolve the file.
2. Verify that it is not a directory.
3. Check that the file is not currently open.
4. Remove its directory entry.
5. Release all data blocks.
6. Release indirect blocks.
7. Release the inode.

Unlinking a directory returns:

```text
EISDIR
```

Deleting an opened file is rejected.

---

# 12. File Statistics

`ufs_stat()` retrieves metadata about a filesystem object.

The information is obtained from the object's inode.

Typical information includes:

- Object type.
- File size.
- Number of blocks.
- Other filesystem metadata.

For example:

```text
stat("/docs/course/notes.txt")
```

first resolves the path and then reads the corresponding inode.

If the path does not exist:

```text
ENOENT
```

---

# 13. File Descriptors

The filesystem supports a maximum of **32 simultaneous open descriptors**.

Each descriptor contains:

```text
+----------------------+
| Active / inactive    |
+----------------------+
| Inode number         |
+----------------------+
| Current offset       |
+----------------------+
| Open flags           |
+----------------------+
```

The descriptor table is reset during mount.

---

## 13.1 Descriptor Allocation

When `ufs_open()` is called:

1. Find a free descriptor slot.
2. Verify that the target exists.
3. Verify that the target is a regular file.
4. Store the inode number.
5. Set the initial offset to zero.
6. Store the open flags.
7. Mark the descriptor active.

If all 32 descriptors are already in use, opening another file must fail.

---

## 13.2 Descriptor Validation

Every descriptor-based operation must validate the descriptor before use.

Invalid or closed descriptors must return an appropriate error instead of accessing invalid memory.

Operations requiring validation include:

- `ufs_read()`
- `ufs_write()`
- `ufs_seek()`
- `ufs_close()`

---

# 14. Opening and Closing Files

## 14.1 `ufs_open()`

`ufs_open()` opens an existing regular file.

Opening a directory as a regular file is rejected.

The supported access modes are:

```text
Read only
Write only
Read / write
```

Append mode is also supported.

The initial descriptor offset is:

```text
0
```

unless append behavior determines the write position.

---

## 14.2 `ufs_close()`

`ufs_close()` marks a descriptor as inactive.

The descriptor slot becomes available for reuse.

Closing an invalid or already closed descriptor must fail.

---

# 15. Reading

`ufs_read()` reads bytes starting at the descriptor's current offset.

The read operation must:

1. Validate the descriptor.
2. Verify that reading is permitted.
3. Read from the current offset.
4. Translate file offsets to filesystem blocks.
5. Handle partial blocks.
6. Handle multiple block boundaries.
7. Stop at EOF.
8. Advance the descriptor offset.

---

## 15.1 Reading at EOF

When the current offset is at or beyond the file size:

```text
read() → 0
```

This indicates end-of-file.

---

## 15.2 Block-Boundary Reads

The implementation must correctly handle reads crossing 512-byte boundaries.

The following sizes are explicitly tested:

```text
0 bytes
1 byte
511 bytes
512 bytes
513 bytes
1500 bytes
```

---

# 16. Writing

`ufs_write()` writes bytes beginning at the descriptor's current offset.

The operation must:

1. Validate the descriptor.
2. Verify that writing is permitted.
3. Determine the correct write offset.
4. Allocate required data blocks.
5. Write data across block boundaries.
6. Update the file size.
7. Advance the descriptor offset.

---

## 16.1 Writing Across Blocks

Writing must support operations that cross filesystem block boundaries.

Examples include:

```text
511 bytes
512 bytes
513 bytes
1500 bytes
```

The implementation must correctly split the operation into block-sized portions.

---

## 16.2 Writing After EOF

If the descriptor seeks beyond EOF and then writes:

```text
current offset > file size
```

the gap must be zero-filled.

Example:

```text
Existing file:

AAAA

Seek to offset 10

Write:

BBBB
```

The resulting file contains:

```text
AAAA\0\0\0\0\0\0BBBB
```

---

## 16.3 Append Mode

In append mode, every write starts at the current end of the file.

Therefore:

```text
offset = file size
```

is selected before each write.

The descriptor offset is then advanced after the write.

---

## 16.4 ENOSPC During Write

If there is not enough free space for a write:

```text
errno = ENOSPC
```

The implementation must avoid leaking blocks.

Allocated metadata must remain consistent.

---

# 17. Seeking

`ufs_seek()` changes the current descriptor offset.

The supported operations are:

```text
SEEK_SET
SEEK_CUR
SEEK_END
```

---

## 17.1 SEEK_SET

The new offset is:

```text
offset = requested_offset
```

---

## 17.2 SEEK_CUR

The new offset is:

```text
offset = current_offset + requested_offset
```

---

## 17.3 SEEK_END

The new offset is:

```text
offset = file_size + requested_offset
```

---

## 17.4 Invalid Offsets

Negative final offsets are rejected.

Seeking beyond EOF is allowed.

Seeking beyond EOF does **not** allocate blocks.

Blocks are allocated only when a later write requires them.

---

# 18. Persistence

Filesystem metadata must persist in the disk image.

The following information must survive:

- Superblock.
- Inode bitmap.
- Block bitmap.
- Inode table.
- Directory entries.
- File contents.

A typical persistence test is:

```text
Format image
     |
     v
Mount
     |
     v
Create file
     |
     v
Write data
     |
     v
Unmount
     |
     v
Mount again
     |
     v
Read file
     |
     v
Verify original data
```

The data and metadata must remain correct after remounting.

---

# 19. Error Handling

The filesystem uses standard `errno` values to report errors.

Important required errors include:

| Operation | Error |
|---|---|
| Create duplicate name | `EEXIST` |
| Access missing path | `ENOENT` |
| Traverse through a file | `ENOTDIR` |
| `unlink()` a directory | `EISDIR` |
| Remove non-empty directory | `ENOTEMPTY` |
| Invalid path | `EINVAL` |
| No free inode | `ENOSPC` |
| No free data block | `ENOSPC` |
| Invalid descriptor | `EBADF` |
| Read using write-only descriptor | `EBADF` |
| Write using read-only descriptor | `EBADF` |
| Mount while already mounted | appropriate state error |
| Unmount when not mounted | appropriate state error |

The implementation must return errors consistently and must not leave the filesystem in an inconsistent state.

---

# 20. Integration Between Members

The filesystem is divided into multiple cooperating components.

```text
                    +----------------+
                    |   Public API   |
                    +--------+-------+
                             |
          +------------------+------------------+
          |                  |                  |
          v                  v                  v
   Lifecycle Layer     Namespace Layer     File I/O Layer
          |                  |                  |
          +------------------+------------------+
                             |
                             v
                    Allocation Layer
                             |
                             v
                     Storage Layer
                             |
                             v
                      Disk Image
```

### Member 1

Responsible for:

- Filesystem architecture.
- Disk layout.
- Formatting.
- Mounting.
- Unmounting.
- Mounted filesystem state.

### Member 2

Responsible for:

- Bitmaps.
- Inode management.
- Data-block allocation.
- Direct and indirect block mapping.
- Truncation.

### Member 3

Responsible for:

- Path validation.
- Path resolution.
- Parent resolution.
- Directories.
- Directory entries.
- File creation and deletion.
- Namespace operations.

### Member 4

Responsible for:

- File descriptors.
- Opening and closing files.
- Reading.
- Writing.
- Seeking.

### Member 5

Responsible for:

- Integration testing.
- Regression testing.
- Documentation.
- Demonstration.
- Validation of all required behaviors.

All members' code must compile and work together as one filesystem implementation.

---

# 21. Testing Strategy

The filesystem should be tested using independent test groups.

Each group should preferably use a fresh disk image so that one failed test does not corrupt later tests.

The tests should verify both:

```text
Return value
```

and:

```text
errno
```

when an operation is expected to fail.

Binary data should be used for file I/O tests rather than testing only text strings.

---

# 22. Lifecycle Acceptance Test

The first acceptance test verifies that the filesystem can be formatted, mounted, unmounted, and mounted again.

```text
Format 1 MiB image
        |
        v
     mount
        |
        v
     stat("/")
        |
        v
    unmount
        |
        v
      mount
        |
        v
     stat("/")
        |
        v
    unmount
```

### Expected Result

The root directory exists both before and after remounting.

The root inode remains inode `0`.

---

# 23. Namespace Acceptance Test

The namespace acceptance test verifies nested paths and directory operations.

```text
mkdir /docs
mkdir /docs/course
create /docs/course/notes.txt
list /docs/course
stat /docs/course/notes.txt
unlink /docs/course/notes.txt
rmdir /docs/course
rmdir /docs
```

Expected behavior:

- `/docs` is created successfully.
- `/docs/course` is created successfully.
- `notes.txt` is created successfully.
- The file appears in the directory listing.
- `stat()` returns valid metadata.
- The file can be removed.
- The empty directories can then be removed.

---

# 24. File I/O Acceptance Tests

The file I/O implementation must test:

```text
0 bytes
1 byte
511 bytes
512 bytes
513 bytes
1500 bytes
```

The tests must verify:

- Correct data.
- Correct file size.
- Correct descriptor offset.
- Correct behavior across block boundaries.

---

## 24.1 Seek Tests

Test:

```text
SEEK_SET
SEEK_CUR
SEEK_END
```

Also test:

```text
Seek beyond EOF
Write after seeking beyond EOF
Read at EOF
```

---

## 24.2 Access Mode Tests

Test:

```text
Read only
Write only
Read / write
Append mode
```

Verify that:

- Reading through a write-only descriptor fails.
- Writing through a read-only descriptor fails.
- Append writes occur at EOF.

---

# 25. Descriptor Acceptance Tests

The descriptor table contains 32 entries.

The test must:

1. Open 32 files successfully.
2. Attempt to open a 33rd file.
3. Verify that the 33rd open fails.
4. Close descriptors.
5. Verify that descriptor slots can be reused.

Example:

```text
Open file 1
Open file 2
...
Open file 32
        |
        v
Open file 33 → failure
```

---

# 26. Allocation Acceptance Tests

The allocation system must verify:

### Block Reuse

```text
Allocate block
     |
     v
Free block
     |
     v
Allocate again
```

The released block must become available again.

### Block Exhaustion

Allocate all available data blocks.

The next allocation must return:

```text
ENOSPC
```

### Inode Exhaustion

Allocate all available non-root inodes.

The next inode allocation must return:

```text
ENOSPC
```

---

# 27. Truncation Acceptance Tests

The truncation tests must verify:

### Grow

```text
Create file
     |
     v
Write data
     |
     v
Grow file
     |
     v
Verify new bytes are zero
```

### Shrink

```text
Create large file
     |
     v
Shrink file
     |
     v
Verify size
     |
     v
Verify blocks are released
```

### Truncate to Zero

```text
Create file
     |
     v
Write data
     |
     v
truncate(file, 0)
     |
     v
Verify size = 0
     |
     v
Verify blocks are free
```

---

# 28. Persistence Acceptance Test

The filesystem must preserve its state across unmount and mount.

Example:

```text
Format
  |
  v
Mount
  |
  v
Create /docs
  |
  v
Create /docs/file.txt
  |
  v
Write binary data
  |
  v
Unmount
  |
  v
Mount
  |
  v
Read /docs/file.txt
  |
  v
Compare data
```

The data must be identical after remounting.

Bitmap information must also remain correct.

---

# 29. Corrupted Image Test

The mount implementation must reject corrupted or incompatible images.

The test should modify the superblock and verify that mounting fails.

Examples include:

- Invalid magic number.
- Invalid version.
- Invalid block size.
- Invalid total block count.
- Invalid filesystem layout.

A corrupted image must never be mounted as a valid filesystem.

---

# 30. Compilation

The filesystem should compile using:

```bash
gcc -std=c11 -Wall -Wextra -Werror \
    userfs.c test_userfs.c -o test_userfs
```

The test program is then executed with:

```bash
./test_userfs
```

---

# 31. Sanitizer Testing

AddressSanitizer and UndefinedBehaviorSanitizer should also be used.

Compile with:

```bash
gcc -std=c11 -Wall -Wextra \
    -fsanitize=address,undefined \
    userfs.c test_userfs.c -o test_userfs_san
```

Run:

```bash
./test_userfs_san
```

Sanitizer testing helps detect:

- Memory leaks.
- Buffer overflows.
- Invalid memory accesses.
- Use-after-free.
- Undefined behavior.

---

# 32. Final Acceptance Checklist

The complete implementation should satisfy all of the following:

- [ ] Create a valid 1 MiB filesystem image.
- [ ] Correctly initialize the superblock.
- [ ] Correctly initialize the inode bitmap.
- [ ] Correctly initialize the block bitmap.
- [ ] Create root inode `0`.
- [ ] Mount a valid filesystem.
- [ ] Reject an invalid filesystem image.
- [ ] Reject double mounting.
- [ ] Unmount successfully.
- [ ] Preserve data across remount.
- [ ] Allocate and release inodes.
- [ ] Allocate and release data blocks.
- [ ] Support direct blocks.
- [ ] Support single-indirect blocks.
- [ ] Support double-indirect blocks.
- [ ] Support file truncation.
- [ ] Resolve absolute paths.
- [ ] Create directories.
- [ ] Remove empty directories.
- [ ] Reject removal of non-empty directories.
- [ ] Create regular files.
- [ ] Remove regular files.
- [ ] List directory contents.
- [ ] Return file statistics.
- [ ] Open regular files.
- [ ] Close descriptors.
- [ ] Support 32 simultaneous descriptors.
- [ ] Reject the 33rd descriptor.
- [ ] Read file contents.
- [ ] Write file contents.
- [ ] Support block-boundary operations.
- [ ] Support seeking.
- [ ] Support append mode.
- [ ] Support writing after EOF.
- [ ] Return correct `errno` values.
- [ ] Pass sanitizer testing.

---

# 33. Final Expected System

The completed filesystem should behave as a persistent filesystem stored inside a 1 MiB Linux image.

The complete lifecycle is:

```text
                    +----------------+
                    |  ufs_format()  |
                    +-------+--------+
                            |
                            v
                    +----------------+
                    |   Disk Image   |
                    +-------+--------+
                            |
                            v
                    +----------------+
                    |  ufs_mount()   |
                    +-------+--------+
                            |
                            v
              +---------------------------+
              |    Mounted Filesystem     |
              +-------------+-------------+
                            |
          +-----------------+-----------------+
          |                 |                 |
          v                 v                 v
     Directories         Files          File Descriptors
          |                 |                 |
          +-----------------+-----------------+
                            |
                            v
                     ufs_unmount()
                            |
                            v
                    Persistent Image
```

The main design goal is that all filesystem state is stored consistently in the disk image, while temporary runtime state such as mounted status and open descriptors is maintained in memory.

After unmounting and mounting the same image again, the filesystem must recover its previous state correctly.



