# UserFS Version 1 Design

## 1. Goals

UserFS is a teaching filesystem stored in one Linux disk-image file. The image
is authoritative: files, directories, content, sizes, allocation metadata, and
the root inode survive unmount and remount. Runtime descriptor offsets do not.

The implementation uses one public header (`userfs.h`) and one shared private
contract (`ufs_internal.h`). Every internal and public function returns `0` or a
non-negative result on success, and `-1` with `errno` set on failure.

## 2. Fixed image layout

The image contains 2,048 blocks of 512 bytes, for exactly 1 MiB.

| Blocks | Count | Region |
| --- | ---: | --- |
| 0 | 1 | Superblock |
| 1 | 1 | Inode bitmap |
| 2 | 1 | Physical block bitmap |
| 3-34 | 32 | Inode table |
| 35-2047 | 2,013 | Data and pointer blocks |

The block bitmap indexes physical image block numbers. During format, bits
0-34 are permanently marked used; bits 35-2047 begin free.

## 3. Persistent structures

### Superblock

The 512-byte superblock stores the magic/version, layout positions and sizes,
root inode number, inode capacity, and current free inode/block counters. Mount
validates every fixed layout field and rejects an image whose host-file size is
not exactly 1 MiB.

### Inodes

Each inode is exactly 64 bytes. A 512-byte inode-table block therefore holds
eight inodes, and 32 inode-table blocks hold 256 inodes.

An inode stores:

- type and flags;
- 64-bit byte size;
- number of allocated data blocks;
- eight direct block pointers;
- one single-indirect pointer;
- one double-indirect pointer;
- one reserved word.

Unused block pointers contain `UFS_INVALID_BLOCK` (`UINT32_MAX`), never zero.
Inode 0 is permanently allocated to `/`.

### Directory entries

Each directory entry is exactly 64 bytes and contains a used bit, inode number,
cached type, and a null-terminated name of at most 31 characters. Eight entries
fit in one block. Version 1 directories use eight direct blocks, so one
directory can contain at most 64 entries.

## 4. Allocation

The inode bitmap uses one bit per inode. The physical block bitmap uses one bit
per image block. Allocation scans for the first free bit, marks it, zeroes or
initializes the object, updates the superblock counter, and writes the changed
metadata to the image.

New data blocks are zero-filled. New pointer blocks are filled with
`UFS_INVALID_BLOCK`, because zero is a real physical block number.

File block mapping is:

1. logical blocks 0-7: direct pointers;
2. the next 128 blocks: one single-indirect pointer block;
3. later blocks: one double-indirect root pointing to up to 128 second-level
   pointer blocks.

The 1 MiB image normally reaches `ENOSPC` before the theoretical pointer-tree
limit.

## 5. Directories and paths

Paths must be absolute, at most 255 characters, and canonical: empty
components, trailing slashes (except `/`), `.` and `..` are rejected. Each
component may contain at most 31 characters.

Resolution starts at inode 0 and searches one directory entry at a time.
Creating a node allocates and initializes its inode before inserting the parent
entry. Removing a file or empty directory removes its entry and frees every
data/pointer block and the inode bitmap bit. The code attempts to restore the
directory entry if the later free operation fails.

## 6. Descriptors and I/O

The descriptor table contains 32 in-memory entries. Each entry stores an
in-use flag, inode number, private byte offset, and open flags. It is reset on
mount and unmount and is never written to the image.

Read clamps requests to EOF and splits them across 512-byte blocks. Write grows
the file first, zero-fills newly exposed space, and performs read-modify-write
for partial blocks so unrelated bytes remain unchanged. An aligned full-block
write does not need the preliminary read.

Append chooses the inode's current size at every write. Seek supports
`SEEK_SET`, `SEEK_CUR`, and `SEEK_END`, rejects negative/overflowed final
positions, and never changes file size by itself.

## 7. Important invariants

- Only blocks 35-2047 may be allocated as data/pointer blocks.
- Every allocated inode bit corresponds to an initialized inode.
- Every allocated data/pointer block has its block-bitmap bit set.
- Unused inode and pointer-block entries equal `UFS_INVALID_BLOCK`.
- `inode.size` is the visible byte length; `inode.block_count` counts data
  blocks needed for that length.
- A directory inode's size is its allocated directory storage in bytes.
- Root inode 0 cannot be freed.
- A regular file cannot be unlinked while a UserFS descriptor references it.
- Successful metadata and data changes are written to the disk image before a
  successful unmount returns.

## 8. Consistency scope

Version 1 protects ordinary error paths and rolls back allocation when a grow
fails with `ENOSPC`. It is not crash-consistent: power loss between multiple
metadata writes can leave the image inconsistent because journaling is outside
the assignment. Multi-process and multi-threaded access are also unsupported.

