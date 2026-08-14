#define _POSIX_C_SOURCE 200809L

#include "metadata.h"
#include "journal.h"
#include "ufs_internal.h"
#include "userfs_storage.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* These will be supplied by the other members. */
extern int ufs_operation_read_lock(void);
extern int ufs_operation_write_lock(void);
extern void ufs_operation_unlock(void);

extern int resolve_path_ex(const char *path,
                           int flags,
                           uint32_t *inode_number);

extern int ufs_tx_stage_inode(struct ufs_transaction *tx,
                              uint32_t inode_number,
                              const struct ufs_inode *inode);

/* ============================================================
 * UserFS umask
 * ============================================================ */

static mode_t current_ufs_umask = 0022;
static pthread_mutex_t umask_mutex = PTHREAD_MUTEX_INITIALIZER;

mode_t ufs_set_umask(mode_t new_mask)
{
    mode_t old_mask;

    if (pthread_mutex_lock(&umask_mutex) != 0) {
        return current_ufs_umask;
    }

    old_mask = current_ufs_umask;
    current_ufs_umask = new_mask & 0777;

    (void)pthread_mutex_unlock(&umask_mutex);

    return old_mask;
}

static mode_t get_ufs_umask(void)
{
    mode_t mask;

    (void)pthread_mutex_lock(&umask_mutex);
    mask = current_ufs_umask;
    (void)pthread_mutex_unlock(&umask_mutex);

    return mask;
}

/* ============================================================
 * Timestamp helpers
 * ============================================================ */

int ufs_get_current_disk_time(struct ufs_disk_time *result)
{
    struct timespec now;

    if (result == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (clock_gettime(CLOCK_REALTIME, &now) < 0) {
        return -1;
    }

    result->sec = (int64_t)now.tv_sec;
    result->nsec = (int32_t)now.tv_nsec;

    return 0;
}

void ufs_disk_time_to_timespec(const struct ufs_disk_time *disk_time,
                               struct timespec *result)
{
    if (disk_time == NULL || result == NULL) {
        return;
    }

    result->tv_sec = (time_t)disk_time->sec;
    result->tv_nsec = (long)disk_time->nsec;
}

void ufs_timespec_to_disk_time(const struct timespec *time,
                               struct ufs_disk_time *result)
{
    if (time == NULL || result == NULL) {
        return;
    }

    result->sec = (int64_t)time->tv_sec;
    result->nsec = (int32_t)time->tv_nsec;
}

void ufs_touch_atime(struct ufs_inode *inode)
{
    struct ufs_disk_time now;

    if (inode == NULL) {
        return;
    }

    if (ufs_get_current_disk_time(&now) == 0) {
        inode->atime = now;
    }
}

void ufs_touch_mtime_ctime(struct ufs_inode *inode)
{
    struct ufs_disk_time now;

    if (inode == NULL) {
        return;
    }

    if (ufs_get_current_disk_time(&now) == 0) {
        inode->mtime = now;
        inode->ctime = now;
    }
}

void ufs_touch_ctime(struct ufs_inode *inode)
{
    struct ufs_disk_time now;

    if (inode == NULL) {
        return;
    }

    if (ufs_get_current_disk_time(&now) == 0) {
        inode->ctime = now;
    }
}

static int disk_time_compare(const struct ufs_disk_time *a,
                             const struct ufs_disk_time *b)
{
    if (a->sec < b->sec) {
        return -1;
    }

    if (a->sec > b->sec) {
        return 1;
    }

    if (a->nsec < b->nsec) {
        return -1;
    }

    if (a->nsec > b->nsec) {
        return 1;
    }

    return 0;
}

int ufs_should_update_atime(const struct ufs_inode *inode)
{
    struct ufs_disk_time now;
    int64_t age;

    if (inode == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (ufs_get_current_disk_time(&now) < 0) {
        return -1;
    }

    /*
     * relatime:
     *
     * atime < mtime
     * OR atime < ctime
     * OR atime is at least 24 hours old
     */

    if (disk_time_compare(&inode->atime, &inode->mtime) < 0) {
        return 1;
    }

    if (disk_time_compare(&inode->atime, &inode->ctime) < 0) {
        return 1;
    }

    age = (int64_t)now.sec - (int64_t)inode->atime.sec;

    if (age >= 24 * 60 * 60) {
        return 1;
    }

    return 0;
}

/* ============================================================
 * Permission helpers
 * ============================================================ */

static int valid_access_mode(int requested_access)
{
    if (requested_access & ~(R_OK | W_OK | X_OK | F_OK)) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

static int group_contains(gid_t inode_gid,
                          gid_t caller_gid,
                          const gid_t *supplementary_groups,
                          size_t supplementary_group_count)
{
    size_t i;

    if (caller_gid == inode_gid) {
        return 1;
    }

    for (i = 0; i < supplementary_group_count; ++i) {
        if (supplementary_groups[i] == inode_gid) {
            return 1;
        }
    }

    return 0;
}

int ufs_mode_allows(const struct ufs_inode *inode,
                    uid_t caller_uid,
                    gid_t caller_gid,
                    const gid_t *supplementary_groups,
                    size_t supplementary_group_count,
                    int requested_access)
{
    unsigned int permission_bits;
    unsigned int required = 0;

    if (inode == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (valid_access_mode(requested_access) < 0) {
        return -1;
    }

    /* F_OK only checks existence. */
    if (requested_access == F_OK) {
        return 0;
    }

    /*
     * Project simplification:
     * UID 0 bypasses permission checks.
     */
    if (caller_uid == 0) {
        return 0;
    }

    /*
     * Owner bits: bits 8-6
     * Group bits: bits 5-3
     * Other bits: bits 2-0
     */
    if (caller_uid == (uid_t)inode->uid) {
        permission_bits = (inode->mode >> 6) & 07;
    } else if (group_contains((gid_t)inode->gid,
                              caller_gid,
                              supplementary_groups,
                              supplementary_group_count)) {
        permission_bits = (inode->mode >> 3) & 07;
    } else {
        permission_bits = inode->mode & 07;
    }

    if (requested_access & R_OK) {
        required |= 04;
    }

    if (requested_access & W_OK) {
        required |= 02;
    }

    if (requested_access & X_OK) {
        required |= 01;
    }

    if ((permission_bits & required) != required) {
        errno = EACCES;
        return -1;
    }

    return 0;
}

int ufs_check_access_inode(const struct ufs_inode *inode,
                           int requested_access)
{
    gid_t *groups = NULL;
    int group_count;
    int result;
    int saved_errno;

    if (inode == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (valid_access_mode(requested_access) < 0) {
        return -1;
    }

    group_count = getgroups(0, NULL);

    if (group_count < 0) {
        return -1;
    }

    if (group_count > 0) {
        groups = malloc((size_t)group_count * sizeof(*groups));

        if (groups == NULL) {
            errno = ENOMEM;
            return -1;
        }

        if (getgroups(group_count, groups) < 0) {
            saved_errno = errno;
            free(groups);
            errno = saved_errno;
            return -1;
        }
    }

    result = ufs_mode_allows(inode,
                             geteuid(),
                             getegid(),
                             groups,
                             (size_t)group_count,
                             requested_access);

    saved_errno = errno;

    free(groups);

    errno = saved_errno;
    return result;
}

/* ============================================================
 * Metadata initialization
 * ============================================================ */

int ufs_initialize_metadata(struct ufs_inode *inode,
                            uint32_t type,
                            mode_t requested_mode)
{
    mode_t mode;
    mode_t umask_value;
    struct ufs_disk_time now;

    if (inode == NULL) {
        errno = EINVAL;
        return -1;
    }

    umask_value = get_ufs_umask();

    switch (type) {
    case UFS_TYPE_FILE:
        mode = requested_mode & 0777;

        if (mode == 0) {
            mode = 0666;
        }

        mode &= ~umask_value;
        break;

    case UFS_TYPE_DIR:
        mode = requested_mode & 0777;

        if (mode == 0) {
            mode = 0777;
        }

        mode &= ~umask_value;
        break;

    case UFS_TYPE_SYMLINK:
        mode = 0777;
        break;

    default:
        errno = EINVAL;
        return -1;
    }

    if (ufs_get_current_disk_time(&now) < 0) {
        return -1;
    }

    inode->type = type;
    inode->mode = mode;
    inode->uid = (uint32_t)geteuid();
    inode->gid = (uint32_t)getegid();
    inode->link_count = 1;

    inode->atime = now;
    inode->mtime = now;
    inode->ctime = now;

    return 0;
}

/* ============================================================
 * Helper: read inode
 * ============================================================ */

static int get_inode_from_path(const char *path,
                               uint32_t *inode_number,
                               struct ufs_inode *inode)
{
    if (path == NULL || path[0] == '\0' ||
        inode_number == NULL || inode == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (resolve_path_ex(path, 0, inode_number) < 0) {
        return -1;
    }

    if (read_inode(*inode_number, inode) < 0) {
        return -1;
    }

    return 0;
}

/* ============================================================
 * chmod
 * ============================================================ */

int ufs_chmod(const char *path, mode_t mode)
{
    uint32_t inode_number;
    struct ufs_inode inode;
    struct ufs_transaction tx;
    uid_t caller_uid;
    int saved_errno;

    if (!ufs_is_mounted()) {
        errno = ENODEV;
        return -1;
    }

    if (path == NULL || path[0] == '\0' ||
        (mode & ~07777U) != 0) {
        errno = EINVAL;
        return -1;
    }

    if (ufs_operation_write_lock() < 0) {
        return -1;
    }

    if (get_inode_from_path(path, &inode_number, &inode) < 0) {
        saved_errno = errno;
        ufs_operation_unlock();
        errno = saved_errno;
        return -1;
    }

    caller_uid = geteuid();

    if (caller_uid != 0 &&
        caller_uid != (uid_t)inode.uid) {
        ufs_operation_unlock();
        errno = EPERM;
        return -1;
    }

    inode.mode = (inode.mode & ~07777U) |
                 ((uint32_t)mode & 07777U);

    ufs_touch_ctime(&inode);

    if (ufs_tx_begin(&tx) < 0) {
        saved_errno = errno;
        ufs_operation_unlock();
        errno = saved_errno;
        return -1;
    }

    if (ufs_tx_stage_inode(&tx, inode_number, &inode) < 0) {
        saved_errno = errno;
        ufs_tx_abort(&tx);
        ufs_operation_unlock();
        errno = saved_errno;
        return -1;
    }

    if (ufs_tx_commit(&tx) < 0) {
        saved_errno = errno;
        ufs_tx_abort(&tx);
        ufs_operation_unlock();
        errno = saved_errno;
        return -1;
    }

    ufs_operation_unlock();

    return 0;
}

/* ============================================================
 * Group membership helper
 * ============================================================ */

static int caller_is_member_of_gid(gid_t gid)
{
    gid_t *groups = NULL;
    int count;
    int i;
    int result = 0;
    int saved_errno;

    if (getegid() == gid) {
        return 1;
    }

    count = getgroups(0, NULL);

    if (count < 0) {
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    groups = malloc((size_t)count * sizeof(*groups));

    if (groups == NULL) {
        errno = ENOMEM;
        return -1;
    }

    if (getgroups(count, groups) < 0) {
        saved_errno = errno;
        free(groups);
        errno = saved_errno;
        return -1;
    }

    for (i = 0; i < count; ++i) {
        if (groups[i] == gid) {
            result = 1;
            break;
        }
    }

    free(groups);

    return result;
}

/* ============================================================
 * chown
 * ============================================================ */

int ufs_chown(const char *path, uid_t uid, gid_t gid)
{
    uint32_t inode_number;
    struct ufs_inode inode;
    struct ufs_transaction tx;
    uid_t caller_uid;
    int group_member;
    int saved_errno;

    if (!ufs_is_mounted()) {
        errno = ENODEV;
        return -1;
    }

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (ufs_operation_write_lock() < 0) {
        return -1;
    }

    if (get_inode_from_path(path, &inode_number, &inode) < 0) {
        saved_errno = errno;
        ufs_operation_unlock();
        errno = saved_errno;
        return -1;
    }

    caller_uid = geteuid();

    /*
     * Root can change both owner and group.
     */
    if (caller_uid == 0) {
        if (uid != (uid_t)-1) {
            inode.uid = (uint32_t)uid;
        }

        if (gid != (gid_t)-1) {
            inode.gid = (uint32_t)gid;
        }
    } else {
        /*
         * Non-root must own the inode.
         */
        if (caller_uid != (uid_t)inode.uid) {
            ufs_operation_unlock();
            errno = EPERM;
            return -1;
        }

        /*
         * Non-root cannot change owner.
         */
        if (uid != (uid_t)-1 &&
            uid != (uid_t)inode.uid) {
            ufs_operation_unlock();
            errno = EPERM;
            return -1;
        }

        /*
         * Non-root can only change group to a group
         * they belong to.
         */
        if (gid != (gid_t)-1 &&
            gid != (gid_t)inode.gid) {

            group_member = caller_is_member_of_gid(gid);

            if (group_member < 0) {
                saved_errno = errno;
                ufs_operation_unlock();
                errno = saved_errno;
                return -1;
            }

            if (!group_member) {
                ufs_operation_unlock();
                errno = EPERM;
                return -1;
            }

            inode.gid = (uint32_t)gid;
        }
    }

    ufs_touch_ctime(&inode);

    if (ufs_tx_begin(&tx) < 0) {
        saved_errno = errno;
        ufs_operation_unlock();
        errno = saved_errno;
        return -1;
    }

    if (ufs_tx_stage_inode(&tx, inode_number, &inode) < 0) {
        saved_errno = errno;
        ufs_tx_abort(&tx);
        ufs_operation_unlock();
        errno = saved_errno;
        return -1;
    }

    if (ufs_tx_commit(&tx) < 0) {
        saved_errno = errno;
        ufs_tx_abort(&tx);
        ufs_operation_unlock();
        errno = saved_errno;
        return -1;
    }

    ufs_operation_unlock();

    return 0;
}

/* ============================================================
 * access
 * ============================================================ */

int ufs_access(const char *path, int mode)
{
    uint32_t inode_number;
    struct ufs_inode inode;
    int result;
    int saved_errno;

    if (!ufs_is_mounted()) {
        errno = ENODEV;
        return -1;
    }

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (valid_access_mode(mode) < 0) {
        return -1;
    }

    if (ufs_operation_read_lock() < 0) {
        return -1;
    }

    if (get_inode_from_path(path, &inode_number, &inode) < 0) {
        saved_errno = errno;
        ufs_operation_unlock();
        errno = saved_errno;
        return -1;
    }

    result = ufs_check_access_inode(&inode, mode);

    saved_errno = errno;

    ufs_operation_unlock();

    errno = saved_errno;
    return result;
}

/* ============================================================
 * utimens
 * ============================================================ */

static int valid_timespec(const struct timespec *time)
{
    if (time == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (time->tv_nsec < 0 || time->tv_nsec > 999999999L) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

int ufs_utimens(const char *path,
                const struct timespec times[2])
{
    uint32_t inode_number;
    struct ufs_inode inode;
    struct ufs_transaction tx;
    struct ufs_disk_time now;
    uid_t caller_uid;
    int saved_errno;

    if (!ufs_is_mounted()) {
        errno = ENODEV;
        return -1;
    }

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (times != NULL) {
        if (valid_timespec(&times[0]) < 0 ||
            valid_timespec(&times[1]) < 0) {
            return -1;
        }
    }

    if (ufs_operation_write_lock() < 0) {
        return -1;
    }

    if (get_inode_from_path(path, &inode_number, &inode) < 0) {
        saved_errno = errno;
        ufs_operation_unlock();
        errno = saved_errno;
        return -1;
    }

    caller_uid = geteuid();

    if (caller_uid != 0 &&
        caller_uid != (uid_t)inode.uid) {
        ufs_operation_unlock();
        errno = EPERM;
        return -1;
    }

    if (times == NULL) {
        if (ufs_get_current_disk_time(&now) < 0) {
            saved_errno = errno;
            ufs_operation_unlock();
            errno = saved_errno;
            return -1;
        }

        inode.atime = now;
        inode.mtime = now;
    } else {
        ufs_timespec_to_disk_time(&times[0], &inode.atime);
        ufs_timespec_to_disk_time(&times[1], &inode.mtime);
    }

    ufs_touch_ctime(&inode);

    if (ufs_tx_begin(&tx) < 0) {
        saved_errno = errno;
        ufs_operation_unlock();
        errno = saved_errno;
        return -1;
    }

    if (ufs_tx_stage_inode(&tx, inode_number, &inode) < 0) {
        saved_errno = errno;
        ufs_tx_abort(&tx);
        ufs_operation_unlock();
        errno = saved_errno;
        return -1;
    }

    if (ufs_tx_commit(&tx) < 0) {
        saved_errno = errno;
        ufs_tx_abort(&tx);
        ufs_operation_unlock();
        errno = saved_errno;
        return -1;
    }

    ufs_operation_unlock();

    return 0;
}

/* ============================================================
 * stat conversion helper
 * ============================================================ */

int ufs_fill_stat_from_inode(const struct ufs_inode *inode,
                             struct ufs_stat *result)
{
    if (inode == NULL || result == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(result, 0, sizeof(*result));

    result->type = (int)inode->type;
    result->size = inode->size;
    result->blocks = inode->block_count;
    result->mode = (mode_t)inode->mode;
    result->uid = (uid_t)inode->uid;
    result->gid = (gid_t)inode->gid;
    result->link_count = inode->link_count;

    ufs_disk_time_to_timespec(&inode->atime,
                              &result->atime);

    ufs_disk_time_to_timespec(&inode->mtime,
                              &result->mtime);

    ufs_disk_time_to_timespec(&inode->ctime,
                              &result->ctime);

    return 0;
}
