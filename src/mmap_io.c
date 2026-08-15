#define _GNU_SOURCE

#include "mmap_io.h"
#include "file_io.h"
#include "journal.h"
#include "metadata.h"
#include "ufs_internal.h"
#include "userfs.h"
#include "userfs_storage.h"
#include "ufs_sync.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define UFS_MAX_MAPPINGS 32
#define UFS_PTRS_PER_BLOCK (UFS_BLOCK_SIZE / sizeof(uint32_t))
#define UFS_MAX_MAPPED_FILE_SIZE \
    ((uint64_t)(UFS_DIRECT_BLOCKS + UFS_PTRS_PER_BLOCK + \
                UFS_PTRS_PER_BLOCK * UFS_PTRS_PER_BLOCK) * UFS_BLOCK_SIZE)

struct ufs_mapping {
    int in_use;
    void *address;
    size_t length;
    off_t file_offset;
    uint32_t inode_number;
    uint32_t inode_generation;
    int protection;
    int flags;
};

static struct ufs_mapping mapping_table[UFS_MAX_MAPPINGS];
static pthread_mutex_t mapping_mutex = PTHREAD_MUTEX_INITIALIZER;

static int lock_mappings(void)
{
    int error = pthread_mutex_lock(&mapping_mutex);
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}

static void unlock_mappings(void)
{
    (void)pthread_mutex_unlock(&mapping_mutex);
}

static int persist_inode(uint32_t inode_number,
                         const struct ufs_inode *inode)
{
    struct ufs_transaction tx;

    if (ufs_tx_begin(&tx) < 0 ||
        ufs_tx_stage_inode(&tx, inode_number, inode) < 0 ||
        ufs_tx_commit(&tx) < 0) {
        ufs_tx_abort(&tx);
        return -1;
    }
    return 0;
}

static ssize_t read_inode_range(uint32_t inode_number, uint32_t generation,
                                void *buffer, size_t count, off_t offset)
{
    struct ufs_inode inode;
    uint8_t block[UFS_BLOCK_SIZE];
    size_t total = 0;
    uint64_t available;

    if (read_inode(inode_number, &inode) < 0) {
        return -1;
    }
    if (inode.generation != generation) {
        errno = ESTALE;
        return -1;
    }
    if (inode.type != UFS_TYPE_FILE) {
        errno = EINVAL;
        return -1;
    }
    if ((uint64_t)offset >= inode.size) {
        return 0;
    }

    available = inode.size - (uint64_t)offset;
    if ((uint64_t)count > available) {
        count = (size_t)available;
    }
    while (total < count) {
        uint64_t position = (uint64_t)offset + total;
        uint32_t logical = (uint32_t)(position / UFS_BLOCK_SIZE);
        size_t inside = (size_t)(position % UFS_BLOCK_SIZE);
        size_t chunk = UFS_BLOCK_SIZE - inside;
        uint32_t physical;
        int block_result;

        if (chunk > count - total) {
            chunk = count - total;
        }
        block_result = get_inode_data_block(&inode, logical, &physical);
        if (block_result < 0) {
            return -1;
        }
        if (physical == UFS_INVALID_BLOCK) {
            errno = EIO;
            return -1;
        }
        if (ufs_read_block(physical, block) < 0) {
            return -1;
        }
        memcpy((uint8_t *)buffer + total, block + inside, chunk);
        total += chunk;
    }

    if (total > 0 && ufs_should_update_atime(&inode)) {
        int saved_errno = errno;
        ufs_touch_atime(&inode);
        (void)persist_inode(inode_number, &inode);
        errno = saved_errno;
    }
    return (ssize_t)total;
}

static ssize_t write_inode_range(uint32_t inode_number, uint32_t generation,
                                 const void *buffer, size_t count,
                                 off_t offset)
{
    struct ufs_inode inode;
    uint8_t block[UFS_BLOCK_SIZE];
    uint64_t end;
    size_t total = 0;

    if ((uint64_t)count > UINT64_MAX - (uint64_t)offset) {
        errno = EFBIG;
        return -1;
    }
    end = (uint64_t)offset + (uint64_t)count;
    if (end > UFS_MAX_MAPPED_FILE_SIZE) {
        errno = EFBIG;
        return -1;
    }
    if (read_inode(inode_number, &inode) < 0) {
        return -1;
    }
    if (inode.generation != generation) {
        errno = ESTALE;
        return -1;
    }
    if (inode.type != UFS_TYPE_FILE) {
        errno = EINVAL;
        return -1;
    }
    if (end > inode.size &&
        ufs_truncate_inode(inode_number, &inode, (size_t)end) < 0) {
        return -1;
    }

    while (total < count) {
        uint64_t position = (uint64_t)offset + total;
        uint32_t logical = (uint32_t)(position / UFS_BLOCK_SIZE);
        size_t inside = (size_t)(position % UFS_BLOCK_SIZE);
        size_t chunk = UFS_BLOCK_SIZE - inside;
        uint32_t physical;
        int block_result;

        if (chunk > count - total) {
            chunk = count - total;
        }
        block_result = get_inode_data_block(&inode, logical, &physical);
        if (block_result < 0) {
            return -1;
        }
        if (physical == UFS_INVALID_BLOCK) {
            errno = EIO;
            return -1;
        }
        if (inside == 0 && chunk == UFS_BLOCK_SIZE) {
            memcpy(block, (const uint8_t *)buffer + total, UFS_BLOCK_SIZE);
        } else {
            if (ufs_read_block(physical, block) < 0) {
                return -1;
            }
            memcpy(block + inside, (const uint8_t *)buffer + total, chunk);
        }
        if (ufs_write_block(physical, block) < 0) {
            return -1;
        }
        total += chunk;
    }

    ufs_touch_mtime_ctime(&inode);
    if (persist_inode(inode_number, &inode) < 0) {
        return -1;
    }
    return (ssize_t)total;
}

static int find_mapping(void *address)
{
    int index;

    for (index = 0; index < UFS_MAX_MAPPINGS; ++index) {
        if (mapping_table[index].in_use &&
            mapping_table[index].address == address) {
            return index;
        }
    }
    errno = EINVAL;
    return -1;
}

static int sync_mapping(struct ufs_mapping *mapping, size_t length)
{
    ssize_t written;

    if (mapping->flags == UFS_MAP_PRIVATE ||
        !(mapping->protection & UFS_PROT_WRITE)) {
        return 0;
    }
    written = write_inode_range(mapping->inode_number,
                                mapping->inode_generation,
                                mapping->address, length,
                                mapping->file_offset);
    if (written < 0) {
        return -1;
    }
    if ((size_t)written != length) {
        errno = EIO;
        return -1;
    }
    return ufs_sync_image();
}

static void *ufs_mmap_unlocked(int fd, size_t length, int prot, int flags,
                               off_t offset)
{
    struct file_io_fd_info info;
    struct ufs_inode inode;
    void *address;
    int slot = -1;
    int access_mode;
    int system_prot = 0;
    int index;

    if (!ufs_is_mounted()) {
        errno = ENODEV;
        return UFS_MAP_FAILED;
    }
    if (length == 0 || offset < 0 ||
        (prot & ~(UFS_PROT_READ | UFS_PROT_WRITE)) != 0 ||
        !(prot & UFS_PROT_READ) ||
        (flags != UFS_MAP_SHARED && flags != UFS_MAP_PRIVATE) ||
        (uint64_t)length > UINT64_MAX - (uint64_t)offset ||
        (uint64_t)offset + (uint64_t)length > UFS_MAX_MAPPED_FILE_SIZE) {
        errno = EINVAL;
        return UFS_MAP_FAILED;
    }
    if (file_io_get_fd_info(fd, &info) < 0) {
        return UFS_MAP_FAILED;
    }
    access_mode = info.flags & UFS_O_RDWR;
    if (access_mode == UFS_O_WRONLY ||
        ((prot & UFS_PROT_WRITE) && flags == UFS_MAP_SHARED &&
         access_mode == UFS_O_RDONLY)) {
        errno = EACCES;
        return UFS_MAP_FAILED;
    }
    if (read_inode(info.inode_number, &inode) < 0) {
        return UFS_MAP_FAILED;
    }
    if (inode.generation != info.inode_generation) {
        errno = EBADF;
        return UFS_MAP_FAILED;
    }

    if (lock_mappings() < 0) {
        return UFS_MAP_FAILED;
    }
    for (index = 0; index < UFS_MAX_MAPPINGS; ++index) {
        if (!mapping_table[index].in_use) {
            slot = index;
            break;
        }
    }
    if (slot < 0) {
        unlock_mappings();
        errno = ENOMEM;
        return UFS_MAP_FAILED;
    }

    address = mmap(NULL, length, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (address == MAP_FAILED) {
        unlock_mappings();
        return UFS_MAP_FAILED;
    }
    if (read_inode_range(info.inode_number, info.inode_generation,
                         address, length, offset) < 0) {
        int saved_errno = errno;
        (void)munmap(address, length);
        unlock_mappings();
        errno = saved_errno;
        return UFS_MAP_FAILED;
    }

    if (prot & UFS_PROT_READ) {
        system_prot |= PROT_READ;
    }
    if (prot & UFS_PROT_WRITE) {
        system_prot |= PROT_WRITE;
    }
    if (mprotect(address, length, system_prot) < 0) {
        int saved_errno = errno;
        (void)munmap(address, length);
        unlock_mappings();
        errno = saved_errno;
        return UFS_MAP_FAILED;
    }

    mapping_table[slot].in_use = 1;
    mapping_table[slot].address = address;
    mapping_table[slot].length = length;
    mapping_table[slot].file_offset = offset;
    mapping_table[slot].inode_number = info.inode_number;
    mapping_table[slot].inode_generation = info.inode_generation;
    mapping_table[slot].protection = prot;
    mapping_table[slot].flags = flags;
    unlock_mappings();
    return address;
}

static int ufs_msync_unlocked(void *address, size_t length)
{
    int index;
    int result;

    if (!ufs_is_mounted()) {
        errno = ENODEV;
        return -1;
    }
    if (address == NULL || address == UFS_MAP_FAILED || length == 0) {
        errno = EINVAL;
        return -1;
    }
    if (lock_mappings() < 0) {
        return -1;
    }
    index = find_mapping(address);
    if (index < 0) {
        unlock_mappings();
        return -1;
    }
    if (length > mapping_table[index].length) {
        unlock_mappings();
        errno = EINVAL;
        return -1;
    }
    result = sync_mapping(&mapping_table[index], length);
    unlock_mappings();
    return result;
}

static int ufs_munmap_unlocked(void *address, size_t length)
{
    struct ufs_mapping old_mapping;
    int index;

    if (!ufs_is_mounted()) {
        errno = ENODEV;
        return -1;
    }
    if (address == NULL || address == UFS_MAP_FAILED || length == 0) {
        errno = EINVAL;
        return -1;
    }
    if (lock_mappings() < 0) {
        return -1;
    }
    index = find_mapping(address);
    if (index < 0) {
        unlock_mappings();
        return -1;
    }
    if (length != mapping_table[index].length) {
        unlock_mappings();
        errno = EINVAL;
        return -1;
    }
    if (sync_mapping(&mapping_table[index], length) < 0) {
        unlock_mappings();
        return -1;
    }

    old_mapping = mapping_table[index];
    memset(&mapping_table[index], 0, sizeof(mapping_table[index]));
    if (munmap(old_mapping.address, old_mapping.length) < 0) {
        mapping_table[index] = old_mapping;
        unlock_mappings();
        return -1;
    }
    unlock_mappings();
    return 0;
}

void *ufs_mmap(int fd, size_t length, int prot, int flags, off_t offset)
{
    void *result;
    int saved_errno;

    if (ufs_operation_write_lock() < 0) {
        return UFS_MAP_FAILED;
    }
    result = ufs_mmap_unlocked(fd, length, prot, flags, offset);
    saved_errno = errno;
    ufs_operation_unlock();
    errno = saved_errno;
    return result;
}

int ufs_msync(void *address, size_t length)
{
    int result;
    int saved_errno;

    if (ufs_operation_write_lock() < 0) {
        return -1;
    }
    result = ufs_msync_unlocked(address, length);
    saved_errno = errno;
    ufs_operation_unlock();
    errno = saved_errno;
    return result;
}

int ufs_munmap(void *address, size_t length)
{
    int result;
    int saved_errno;

    if (ufs_operation_write_lock() < 0) {
        return -1;
    }
    result = ufs_munmap_unlocked(address, length);
    saved_errno = errno;
    ufs_operation_unlock();
    errno = saved_errno;
    return result;
}

int ufs_mmap_has_active(void)
{
    int active = 0;
    int index;

    if (lock_mappings() < 0) {
        return 1;
    }
    for (index = 0; index < UFS_MAX_MAPPINGS; ++index) {
        if (mapping_table[index].in_use) {
            active = 1;
            break;
        }
    }
    unlock_mappings();
    return active;
}

int ufs_mmap_inode_is_mapped(uint32_t inode_number)
{
    int mapped = 0;
    int index;

    if (lock_mappings() < 0) {
        return 1;
    }
    for (index = 0; index < UFS_MAX_MAPPINGS; ++index) {
        if (mapping_table[index].in_use &&
            mapping_table[index].inode_number == inode_number) {
            mapped = 1;
            break;
        }
    }
    unlock_mappings();
    return mapped;
}
