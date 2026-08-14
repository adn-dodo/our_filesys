# Recommended Five-Member Split

The original functional split is the right shape, but it needs explicit
interface ownership and integration gates.

## Member 1 - Lead, lifecycle, and integration

- Own `userfs.h` and `ufs_internal.h` interface decisions.
- Implement format, mount, unmount, raw block I/O, and superblock validation.
- Maintain the Makefile and merge modules continuously.
- Reject duplicate public functions and private copies of shared structures.
- Definition of done: every branch links into the complete library.

## Member 2 - Storage and allocation

- Implement inode/block bitmaps, inode table I/O, allocation, reclamation,
  direct/indirect mapping, and truncate.
- Prove `ENOSPC` rollback and free-counter correctness.
- Definition of done: grow, shrink, delete, and reuse blocks on a real image.

## Member 3 - Paths and namespace

- Implement path validation/resolution, directory entry helpers, mkdir/rmdir,
  create/unlink/listdir/stat.
- Use only Member 1/2 interfaces; keep RAM mocks in test files, never in
  production source.
- Definition of done: nested namespace state survives remount.

## Member 4 - Descriptors and file data

- Implement open/close/read/write/seek and descriptor state.
- Test block boundaries, EOF, append, gaps, independent offsets, and access
  modes.
- Definition of done: all operations work through the real storage helpers,
  not only a mock backend.

## Member 5 - QA, documentation, and demonstration

- Write integration and persistence tests from the public API.
- Maintain the error matrix, sanitizer/static-analysis jobs, DESIGN.md,
  README, slides, and demo program.
- Test other members' code independently and report reproducible failures.
- Definition of done: one clean command builds and tests the submitted ZIP.

## Shared integration rules

1. Freeze the public and private contracts before parallel implementation.
2. Internal helpers always return `-1` and set `errno`.
3. Shared on-disk structures exist in one header only.
4. Mock functions exist only in test targets.
5. Merge in this order: lifecycle, storage, namespace, file I/O, integration.
6. Every member adds tests, but Member 5 owns cross-module acceptance tests.
7. Use paired reviews: 1 reviews 2, 2 reviews 4, 4 reviews 3, 3 reviews 1,
   and 5 reviews all public behavior.

