#ifndef _VFS_OPEN_H_
#define _VFS_OPEN_H_

#include <common/types.h>
#include <rendezvos/task/id.h>

#include "vfs_backend_ops.h"
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

/*
 * After lookup/create produced @ino (trunc already applied if needed):
 * permission checks + handle_open. Returns handle|IS_DIR bit or -errno.
 */
i64 vfs_open_install(const vfs_inode_t *ino, i32 flags);

bool vfs_inode_symlink_target(const vfs_inode_t *ino, char *out, u64 cap);
void vfs_join_symlink_target(const char *base, const char *target, char *out,
                             u64 cap);

i64 vfs_lseek_handle(u32 handle, i64 offset, i32 whence);
i64 vfs_fstat_handle(pid_t pid, u32 handle, u64 user_statbuf);

/* Lookup @path into @out; follow one symlink level when @follow_symlink. */
i64 vfs_lookup_path(const char *path, vfs_inode_t *out, bool follow_symlink);

#endif /* _VFS_OPEN_H_ */
