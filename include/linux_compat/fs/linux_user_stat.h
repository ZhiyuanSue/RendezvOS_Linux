#ifndef _LINUX_COMPAT_FS_LINUX_USER_STAT_H_
#define _LINUX_COMPAT_FS_LINUX_USER_STAT_H_

#include <common/types.h>

/*
 * Linux userspace struct stat layout (glibc/musl on x86_64 and aarch64).
 * Differs from servers/fs vfs_kstat_t field order — convert before user copy.
 */
typedef struct linux_user_stat {
        u64 st_dev;
        u64 st_ino;
        u64 st_nlink;
        u32 st_mode;
        u32 st_uid;
        u32 st_gid;
        u32 __pad0;
        u64 st_rdev;
        i64 st_size;
        i32 st_blksize;
        i32 __pad1;
        i64 st_blocks;
        i64 st_atime_sec;
        i64 st_atime_nsec;
        i64 st_mtime_sec;
        i64 st_mtime_nsec;
        i64 st_ctime_sec;
        i64 st_ctime_nsec;
} linux_user_stat_t;

#endif /* _LINUX_COMPAT_FS_LINUX_USER_STAT_H_ */
