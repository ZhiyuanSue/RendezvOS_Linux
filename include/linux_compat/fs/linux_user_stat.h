#ifndef _LINUX_COMPAT_FS_LINUX_USER_STAT_H_
#define _LINUX_COMPAT_FS_LINUX_USER_STAT_H_

#include <common/types.h>

/*
 * Linux userspace struct stat — layout is arch-specific.
 *
 * x86_64 (asm/stat.h):  nlink (u64) before mode (u32)
 * aarch64 (asm-generic/stat.h): mode (u32) before nlink (u32)
 *
 * Writing the x86 layout into an aarch64 glibc busybox makes S_ISDIR fail, so
 * `ls /bin` prints the path as a file instead of listing the directory.
 */

#if defined(_AARCH64_)

typedef struct linux_user_stat {
        u64 st_dev;
        u64 st_ino;
        u32 st_mode;
        u32 st_nlink;
        u32 st_uid;
        u32 st_gid;
        u64 st_rdev;
        u64 __pad1;
        i64 st_size;
        i32 st_blksize;
        i32 __pad2;
        i64 st_blocks;
        i64 st_atime_sec;
        u64 st_atime_nsec;
        i64 st_mtime_sec;
        u64 st_mtime_nsec;
        i64 st_ctime_sec;
        u64 st_ctime_nsec;
        u32 __unused4;
        u32 __unused5;
} linux_user_stat_t;

#elif defined(_X86_64_)

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
        i64 st_blksize;
        i64 st_blocks;
        i64 st_atime_sec;
        i64 st_atime_nsec;
        i64 st_mtime_sec;
        i64 st_mtime_nsec;
        i64 st_ctime_sec;
        i64 st_ctime_nsec;
} linux_user_stat_t;

#else

/* Fallback: prefer asm-generic field order. */
typedef struct linux_user_stat {
        u64 st_dev;
        u64 st_ino;
        u32 st_mode;
        u32 st_nlink;
        u32 st_uid;
        u32 st_gid;
        u64 st_rdev;
        u64 __pad1;
        i64 st_size;
        i32 st_blksize;
        i32 __pad2;
        i64 st_blocks;
        i64 st_atime_sec;
        u64 st_atime_nsec;
        i64 st_mtime_sec;
        u64 st_mtime_nsec;
        i64 st_ctime_sec;
        u64 st_ctime_nsec;
        u32 __unused4;
        u32 __unused5;
} linux_user_stat_t;

#endif

#endif /* _LINUX_COMPAT_FS_LINUX_USER_STAT_H_ */
