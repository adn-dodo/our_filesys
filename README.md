# UserFS

UserFS is a small filesystem implemented as a C library. It runs entirely in
user space and stores all persistent state in one regular Linux file used as a
1 MiB disk image. It does not use FUSE or modify the Linux kernel.

## Implemented public API

- Format, mount, and unmount
- Create, remove, list, and stat nested directories
- Create, unlink, open, close, read, write, seek, and truncate files
- Linux-style `-1`/`errno` error reporting
- Persistence across unmount and remount
- Direct, single-indirect, and double-indirect file block mapping

## Build and test

```bash
make
make test
```

`make test` builds the complete library with strict warnings and runs the real
disk-image integration test. The integration test performs 82 checks, including
persistence, nested paths, block-boundary I/O, zero-filled gaps, append mode,
descriptor limits, truncation, deletion, and an 80,000-byte file that crosses
direct, single-indirect, and double-indirect mappings. It also forces `ENOSPC`
and proves that the original file size and data remain unchanged.

## Visible demonstration without a Makefile

Compile the complete filesystem and demonstration directly with GCC:

```bash
gcc -std=c11 -Wall -Wextra -Werror -pedantic \
    userfs.c userfs_storage.c namespace.c file_io.c demo_userfs.c \
    -o demo_userfs

./demo_userfs
```

The demonstration creates `userfs_demo.img`, builds
`/projects/demo/message.txt` inside it, writes text, unmounts, remounts,
appends more text, remounts again, and prints the persistent content. It leaves
the image on disk for inspection.

The full 82-check suite can also be compiled without a Makefile:

```bash
gcc -std=c11 -Wall -Wextra -Werror -pedantic \
    userfs.c userfs_storage.c namespace.c file_io.c test_integration.c \
    -o test_integration

./test_integration
```

## Interactive presentation shell

For the clearest live presentation, compile the interactive shell directly:

```bash
gcc -std=c11 -Wall -Wextra -Werror -pedantic \
    userfs.c userfs_storage.c namespace.c file_io.c userfs_shell.c \
    -o userfs_shell

./userfs_shell
```

The shell has a virtual UserFS working directory. It supports `cd`, `pwd`,
`ls` with no argument, absolute paths, relative paths, `.`, and `..`.
The prompt shows the current path, for example `userfs:/projects/demo$`.
These commands navigate inside the image and never call the host Linux
`chdir()` or `opendir()`.

The ordinary commands (`mkdir`, `create`/`touch`, `write`, `append`,
`cat`, `ls`, `stat`, `truncate`, `rm`, and `rmdir`) call the public
library API. The
`inode`, `map`, `show`, `bitmap`, and `super` commands are debug-only
inspectors for a presentation; they intentionally read private structures.
`show PATH` produces one compact summary containing the file size, data
blocks, direct pointers, block mapping, bitmap membership, and content.

Run the sanitizer build with:

```bash
make sanitize
```

Trace Member 4's file-I/O operations with:

```bash
make trace
```

## Files

| File | Responsibility |
| --- | --- |
| `userfs.h` | Public library API |
| `ufs_internal.h` | Shared private layout and integration contract |
| `userfs.c` | Image formatting, mounting, raw block I/O, superblock |
| `userfs_storage.c` | Bitmaps, inode allocation, block mapping, truncate |
| `namespace.c` | Paths, directories, create/unlink/stat/listdir |
| `file_io.c` | Descriptor table, open/close/read/write/seek |
| `demo_userfs.c` | Visible end-to-end filesystem demonstration |
| `userfs_shell.c` | Interactive operations plus inode/bitmap inspection |
| `test_integration.c` | Complete real-image acceptance and persistence test |
| `DESIGN.md` | On-disk design, allocation policy, and invariants |
| `INTEGRATION_REVIEW.md` | Findings from the submitted member files and fixes |
| `PRESENTATION_DEMO.md` | Exact live commands and explanations for presenting |
| `SHELL_TEST_SCENARIOS.md` | Interactive scenarios with expected results |

The original isolated member tests are preserved in `original_member_tests/`.
They are historical unit harnesses and are not part of the integrated build,
because some of them replace real storage with RAM mocks or assume a different
helper interface.

## Current Version 1 limits

- Fixed 1 MiB image and 512-byte blocks
- 256 inodes
- At most 32 simultaneously open UserFS descriptors
- File/directory names up to 31 characters; paths up to 255 characters
- Directories use at most eight direct blocks: 64 entries per directory
- One mounted image and one process at a time
- No permissions, links, timestamps, journaling, `mmap`, or concurrency
