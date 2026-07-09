#ifndef _VFS_OPEN_H_
#define _VFS_OPEN_H_

#include <common/types.h>
#include <rendezvos/task/id.h>

#include "vfs_kstat.h"

/* Linux open / openat flag bits (oscomp user headers + x86_64 Linux). */
#define VFS_O_ACCMODE   3
#define VFS_O_RDONLY    0
#define VFS_O_WRONLY    1
#define VFS_O_RDWR      2
#define VFS_O_CREAT     0x40
#define VFS_O_EXCL      0x80
#define VFS_O_TRUNC     0x200
#define VFS_O_APPEND    0x400
#define VFS_O_DIRECTORY 0x10000

#define VFS_AT_FDCWD (-100)

i64 vfs_openat(pid_t pid, i32 dirfd, const char *path, i32 flags, u32 mode);
i64 vfs_read_fd(pid_t pid, i32 fd, u64 user_buf, u64 count);
i64 vfs_write_fd(pid_t pid, i32 fd, u64 user_buf, u64 count);
i64 vfs_lseek_fd(pid_t pid, i32 fd, i64 offset, i32 whence);
i64 vfs_fstat_fd(pid_t pid, i32 fd, u64 user_statbuf);
i64 vfs_statat(pid_t pid, i32 dirfd, const char *path, u64 user_statbuf,
               i32 flags);
i64 vfs_mkdirat(pid_t pid, i32 dirfd, const char *path, u32 mode);
i64 vfs_unlinkat(pid_t pid, i32 dirfd, const char *path, i32 flags);

#endif /* _VFS_OPEN_H_ */
