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

i64 vfs_open_path(const char *path, i32 flags, u32 mode);
i64 vfs_read_handle(pid_t pid, u32 handle, u64 user_buf, u64 count);
i64 vfs_write_handle(pid_t pid, u32 handle, u64 user_buf, u64 count);
i64 vfs_lseek_handle(u32 handle, i64 offset, i32 whence);
i64 vfs_fstat_handle(pid_t pid, u32 handle, u64 user_statbuf);
i64 vfs_stat_path(pid_t pid, const char *path, u64 user_statbuf, i32 flags);
i64 vfs_mkdir_path(const char *path, u32 mode);
i64 vfs_unlink_path(const char *path, i32 flags);
i64 vfs_rename_path(const char *path, const char *newpath, i32 flags);
i64 vfs_link_path(const char *path, const char *newpath, i32 flags);
i64 vfs_validate_dir(const char *path);
i64 vfs_getdents64_handle(pid_t pid, u32 handle, u64 user_dirp, u64 count);
i64 vfs_readlink_path(pid_t pid, const char *path, u64 user_buf, u64 bufsiz);
i64 vfs_faccessat_path(pid_t pid, const char *path, u32 mode, u32 flags);

#endif /* _VFS_OPEN_H_ */
