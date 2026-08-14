# UserFS Interactive Test Scenarios

Compile the shell without a Makefile:

```bash
gcc -std=c11 -Wall -Wextra -Werror -pedantic \
    userfs.c userfs_storage.c namespace.c file_io.c userfs_shell.c \
    -o userfs_shell

./userfs_shell
```

Run each command inside the `userfs:` prompt. Start with Scenario 1 and keep
the same image for the later scenarios.

## Scenario 1: Fresh filesystem and navigation

```text
format
mount
pwd
ls
mkdir projects
cd projects
mkdir demo
cd demo
pwd
ls
```

Expected:

- `pwd` first prints `/`, then prints `/projects/demo`.
- The prompt changes to `userfs:/projects/demo$`.
- The new `demo` directory is initially empty.

## Scenario 2: Create, write, and inspect a file

```text
touch message.txt
write message.txt ABC
ls
show message.txt
```

On a fresh image with this exact creation order, `show` should print:

```text
File size       = 3 bytes
Data blocks     = 1
direct[0]       = 38
Block map       = logical 0 → physical 38
Bitmap includes = 38
File content    = ABC
```

The physical number can differ after a different allocation history. The
important invariant is that `direct[0]`, the map, and the bitmap agree.

## Scenario 3: Append versus replace

```text
append message.txt DEF
cat message.txt
write message.txt XYZ
cat message.txt
```

Expected:

- After `append`, the content is `ABCDEF`.
- `write` replaces the old content, so the final content is `XYZ`.

## Scenario 4: Cross the 512-byte block boundary

```text
truncate message.txt 513
inode message.txt
map message.txt
bitmap
truncate message.txt 3
inode message.txt
map message.txt
bitmap
```

Expected:

- At 513 bytes the inode uses two data blocks because
  `ceil(513 / 512) = 2`.
- After shrinking to 3 bytes it uses one block again.
- The second physical block disappears from the allocation bitmap.

## Scenario 5: Persistence across unmount and mount

```text
unmount
mount
cat /projects/demo/message.txt
cd /projects/demo
pwd
show message.txt
```

Expected:

- Mount starts the shell at `/`.
- The path, inode, mapping, size, and content still exist.
- This proves the authoritative state came from the image.

## Scenario 6: Relative paths, dot, and parent directory

```text
pwd
stat .
ls ..
cd ..
pwd
ls
cd ./demo
pwd
cd ../..
pwd
```

Expected:

- `.` means the current directory.
- `..` moves to the parent.
- The final directory is `/`.

## Scenario 7: Error handling

```text
cd /projects/demo
cd message.txt
cd missing
touch message.txt
rmdir .
cd /
rmdir projects
cat /missing.txt
```

Expected errors:

- `cd message.txt` → `ENOTDIR` (Not a directory).
- `cd missing` and `cat /missing.txt` → `ENOENT`.
- Creating `message.txt` again → `EEXIST`.
- Removing the current directory → `EBUSY`.
- Removing non-empty `projects` → `ENOTEMPTY`.

## Scenario 8: Delete objects and reclaim allocation

```text
bitmap
rm /projects/demo/message.txt
bitmap
rmdir /projects/demo
rmdir /projects
ls /
bitmap
super
```

Expected:

- Removing the file frees its inode and data blocks.
- Removing the directories frees their inodes and directory blocks.
- The root directory may retain its first block as reusable capacity.
- `ls /` finishes empty.

## Scenario 9: Complete automated acceptance test

Leave the shell with `quit`, then run this in the Linux terminal:

```bash
gcc -std=c11 -Wall -Wextra -Werror -pedantic \
    userfs.c userfs_storage.c namespace.c file_io.c test_integration.c \
    -o test_integration

./test_integration
```

Expected final line:

```text
Integration checks passed: 82
```

This suite checks block-boundary reads/writes, independent descriptors, append,
seek, zero-filled gaps, direct and indirect mappings, persistence, `ENOSPC`
rollback, descriptor exhaustion, deletion, and Linux-style `errno` values.
