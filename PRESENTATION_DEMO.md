# Best Live UserFS Presentation

## 1. Compile without a Makefile

```bash
gcc -std=c11 -Wall -Wextra -Werror -pedantic \
    userfs.c userfs_storage.c namespace.c file_io.c userfs_shell.c \
    -o userfs_shell

./userfs_shell
```

The shell uses `userfs_live.img` unless another image name is passed as its
first argument.

## 2. Recommended live command sequence

Enter these commands one at a time. Pause after each inspection command and
explain the observed change.

```text
format
mount
super
bitmap

mkdir /docs
cd docs
mkdir course
cd course
touch notes.txt
pwd
ls
inode notes.txt
bitmap

write notes.txt Hello from our UserFS
stat notes.txt
inode notes.txt
map notes.txt
bitmap
show notes.txt

truncate notes.txt 600
inode notes.txt
map notes.txt
bitmap

truncate notes.txt 21
map notes.txt

unmount
mount
cat /docs/course/notes.txt
append /docs/course/notes.txt -persistent
cat /docs/course/notes.txt

cd /docs/course
rm /docs/course/notes.txt
bitmap
cd /
rmdir /docs/course
rmdir /docs
super
quit
```

If the exact text length differs, use the size printed by `stat` instead of
`21` when shrinking the file back.

The `show PATH` command is the clearest single-screen result. It combines the
file size, block count, direct pointers, logical-to-physical mapping, bitmap
membership, and file content.

## 3. What to explain

### After `format`, `mount`, `super`, and `bitmap`

Say:

> The image contains 2,048 blocks of 512 bytes. Blocks 0-34 are metadata and
> blocks 35-2047 are the data region. At this moment only inode 0, the root
> directory, is allocated. The root has no data block until its first entry is
> created.

### After creating directories and the file

The allocated inodes should be 0, 1, 2, and 3:

- inode 0: `/`;
- inode 1: `/docs`;
- inode 2: `/docs/course`;
- inode 3: `notes.txt`.

The empty file's inode has size 0, block count 0, and unused block pointers.
The allocated data blocks belong to directory-entry tables: the root stores
`docs`, `docs` stores `course`, and `course` stores `notes.txt`.

### After `write`

Say:

> Writing content changes the persistent inode size and allocates physical
> block 38 for logical file block 0. The descriptor offset advances during the
> write, but the descriptor itself remains only in memory.

`inode` shows the size and direct pointer. `map` shows that logical block 0 is
stored in a physical image block. `bitmap` shows the new allocation bit.

### After truncating to 600 bytes

Since one block stores 512 bytes, 600 bytes require two blocks:

```text
ceil(600 / 512) = 2 blocks
```

The first direct pointer maps logical block 0 and the second maps logical block
1. Bytes between the old EOF and byte 599 are zero-filled. Shrinking the file
back frees the second block.

### After unmount and remount

Say:

> Unmount flushes successful changes and closes the Linux image descriptor.
> Mount reads and validates the superblock again. Reading the same path and
> content after remount proves that the namespace, inode, size, block mapping,
> and data came from the image rather than temporary RAM.

### After `rm` and `bitmap`

The file inode and its data block disappear from the allocation bitmaps. Empty
directories can then be removed, releasing their inode and directory blocks.
The root may retain its first directory block as reusable capacity; that is not
an unreachable leak because inode 0 still points to it.

## 4. Important presentation distinction

The normal commands mutate or query the filesystem only through `userfs.h`:

- `mkdir`, `rmdir`, `create`, `rm`, `ls`, and `stat`;
- `write`, `append`, `cat`, and `truncate`;
- `format`, `mount`, and `unmount`.

The following are debug-only inspection commands added for the demonstration:

- `inode`;
- `map`;
- `bitmap`;
- `super`.

They are not additional assignment APIs. They read private structures so the
audience can see what the real library has stored.

## 5. Strong closing sentence

> This demonstration proves both behavior and internal design: the public API
> creates and modifies files, the inspectors show inode and block allocation,
> and remounting proves that the state is persistent inside one disk image.
