#ifndef METADATA_H
#define METADATA_H

#include "ufs_internal.h"

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

int ufs_mode_allows(const struct ufs_inode *inode,
                    uid_t caller_uid,
                    gid_t caller_gid,
                    const gid_t *supplementary_groups,
                    size_t supplementary_group_count,
                    int requested_access);

int ufs_check_access_inode(const struct ufs_inode *inode,
                           int requested_access);

int ufs_initialize_metadata(struct ufs_inode *inode,
                            uint32_t type,
                            mode_t requested_mode);

int ufs_get_current_disk_time(struct ufs_disk_time *result);

void ufs_touch_atime(struct ufs_inode *inode);
void ufs_touch_mtime_ctime(struct ufs_inode *inode);
void ufs_touch_ctime(struct ufs_inode *inode);

void ufs_disk_time_to_timespec(const struct ufs_disk_time *disk_time,
                               struct timespec *result);

void ufs_timespec_to_disk_time(const struct timespec *time,
                               struct ufs_disk_time *result);

int ufs_should_update_atime(const struct ufs_inode *inode);

mode_t ufs_set_umask(mode_t new_mask);

int ufs_chmod(const char *path, mode_t mode);

int ufs_chown(const char *path, uid_t uid, gid_t gid);

int ufs_access(const char *path, int mode);

int ufs_utimens(const char *path,
                const struct timespec times[2]);

int ufs_fill_stat_from_inode(const struct ufs_inode *inode,
                             struct ufs_stat *result);

#endif
