#ifndef UFS_SYNC_H
#define UFS_SYNC_H

int ufs_operation_read_lock(void);
int ufs_operation_write_lock(void);
void ufs_operation_unlock(void);

#endif
