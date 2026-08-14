# Integration Review

## Submitted state

The submitted archive contained promising implementations, but it was not an
integrated filesystem yet.

### Strong work

- The lifecycle code correctly formatted a fixed image, initialized the root,
  validated the superblock, and used exact `pread`/`pwrite` loops.
- The storage module implemented bitmap allocation and ambitious direct,
  single-indirect, and double-indirect mapping.
- The namespace module covered nested resolution and all required public
  namespace operations in its isolated RAM test.
- The file-I/O module had strong descriptor, boundary, append, gap, partial
  block, and seek logic, supported by 35 isolated tests.

### Blocking integration problems found

1. `userfs_storage.c` included a missing `userfs_internal.h`, while the archive
   provided `ufs_internal.h`.
2. `userfs.c` still defined `ENOSYS` public stubs, causing duplicate symbols
   when the namespace and file-I/O implementations were linked.
3. `namespace.c` contained a private RAM inode table and RAM data blocks inside
   production code, so namespace success did not persist to the image.
4. Lifecycle code and file-I/O code owned two different descriptor tables.
5. Modules mixed two helper error conventions: `-errno` versus `-1` plus
   `errno`.
6. `file_io.h` duplicated the inode layout instead of consuming one shared
   private contract.
7. Newly created namespace inodes used zero block pointers; zero is a valid
   block, while unused pointers must be `UINT32_MAX`.
8. Freeing an inode did not release its data and indirect pointer blocks.
9. Truncate did not update `inode.block_count`.
10. The submitted `DESIGN.md` was one byte, `README.md` was only 13 bytes, and
    there was no Makefile.
11. The large `test_userfs.c` had state-order errors: one test left `/docs`
    non-empty but a later test expected removal to succeed, and several I/O
    helpers ran while the image was unmounted. Its Member 4 test function was
    also never called.

## Integration changes

- Established `ufs_internal.h` as the single source of on-disk definitions and
  lifecycle/storage integration prototypes.
- Created one shared mounted-image context and exported checked raw block I/O.
- Removed all production RAM mocks and all `ENOSYS` duplicate stubs.
- Kept exactly one descriptor table in `file_io.c`; lifecycle resets it.
- Standardized all implementation helpers on `-1` plus `errno`.
- Added safe inode initialization, root protection, full block reclamation,
  counter persistence, and block-count maintenance.
- Connected namespace operations to real inode/block allocation.
- Added canonical path validation and missing null/error checks.
- Added a strict Makefile, complete design, README, and real-image integration
  test. Original isolated tests were retained only as historical references.

## Verified result

The integrated source compiles with strict C11 warnings as errors. The
real-image integration suite passes 82 checks. It verifies data and namespace
persistence across remount, an 80,000-byte binary file using all three mapping
levels, truncation, gap zeroing, descriptor behavior, Linux errors, and
deletion/reclamation paths, including an `ENOSPC` rollback check.

## Honest overall assessment

Individually, the members produced several good building blocks. As submitted,
the project was not ready to deliver because it did not compile as one library
and the namespace module was not persistent. After integration, it is a strong
Version 1 assignment implementation. It is still an educational filesystem,
not a production filesystem: there is no journaling, fsck, locking, portable
byte-order encoding, or crash recovery.
