#ifndef _VFS_FD_H_
#define _VFS_FD_H_

#include <common/stdbool.h>
#include <common/types.h>
#include <rendezvos/task/id.h>

#include "vfs_backend_ops.h"

/*
 * Per-process open file table (vfs_server front-end, Step 3).
 * fds 0–2 are reserved for stdio shim in linux_layer; VFS fds start at 3.
 */

#define VFS_FD_MIN       3
#define VFS_FD_MAX       32
#define VFS_FD_PID_SLOTS 32

typedef struct vfs_open_file {
        bool in_use;
        vfs_inode_t ino;
        u64 offset;
        i32 open_flags;
} vfs_open_file_t;

void vfs_fd_init(void);

i64 vfs_fd_open(pid_t pid, const vfs_inode_t *ino, i32 open_flags);
i64 vfs_fd_close(pid_t pid, i32 fd);

vfs_open_file_t *vfs_fd_get(pid_t pid, i32 fd);

void vfs_fd_drop_pid(pid_t pid);

#endif /* _VFS_FD_H_ */
