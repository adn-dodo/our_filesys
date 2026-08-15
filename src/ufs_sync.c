#define _POSIX_C_SOURCE 200809L

#include "ufs_sync.h"
#include "ufs_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>

static pthread_mutex_t operation_lock;
static pthread_once_t operation_lock_once = PTHREAD_ONCE_INIT;
static _Thread_local unsigned int operation_lock_depth;

static void initialize_operation_lock(void)
{
    pthread_mutexattr_t attributes;

    (void)pthread_mutexattr_init(&attributes);
    (void)pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    (void)pthread_mutex_init(&operation_lock, &attributes);
    (void)pthread_mutexattr_destroy(&attributes);
}

static int lock_image(short type)
{
    struct flock lock;

    if (!g_ufs.mounted || g_ufs.fd < 0) {
        errno = ENODEV;
        return -1;
    }

    memset(&lock, 0, sizeof(lock));
    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;

    while (fcntl(g_ufs.fd, F_SETLKW, &lock) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

static int lock_operation(short image_lock_type)
{
    int error;

    if (operation_lock_depth > 0) {
        ++operation_lock_depth;
        return 0;
    }

    error = pthread_once(&operation_lock_once, initialize_operation_lock);
    if (error == 0) {
        error = pthread_mutex_lock(&operation_lock);
    }

    if (error != 0) {
        errno = error;
        return -1;
    }
    if (lock_image(image_lock_type) < 0) {
        (void)pthread_mutex_unlock(&operation_lock);
        return -1;
    }
    operation_lock_depth = 1;
    return 0;
}

int ufs_operation_read_lock(void)
{
    return lock_operation(F_RDLCK);
}

int ufs_operation_write_lock(void)
{
    return lock_operation(F_WRLCK);
}

void ufs_operation_unlock(void)
{
    struct flock lock;

    if (operation_lock_depth == 0) {
        return;
    }
    --operation_lock_depth;
    if (operation_lock_depth != 0) {
        return;
    }

    if (g_ufs.fd >= 0) {
        memset(&lock, 0, sizeof(lock));
        lock.l_type = F_UNLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = 0;
        lock.l_len = 0;
        (void)fcntl(g_ufs.fd, F_SETLK, &lock);
    }
    (void)pthread_mutex_unlock(&operation_lock);
}
