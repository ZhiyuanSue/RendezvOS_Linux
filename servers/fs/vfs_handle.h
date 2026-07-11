#ifndef _VFS_HANDLE_H_
#define _VFS_HANDLE_H_

#include <common/stdbool.h>
#include <common/types.h>

#include "vfs_backend_ops.h"

/*
 * Global open-file handle table (struct file equivalent).
 * Compat layer maps fd → handle; server stores offset/flags/inode.
 */

#define VFS_HANDLE_INVALID 0u
#define VFS_HANDLE_MAX     128u

typedef struct vfs_open_handle {
        bool in_use;
        u32 refcnt;
        vfs_inode_t ino;
        u64 offset;
        i32 open_flags;
} vfs_open_handle_t;

void vfs_handle_init(void);

u32 vfs_handle_open(const vfs_inode_t *ino, i32 open_flags);
i64 vfs_handle_retain(u32 handle);
i64 vfs_handle_close(u32 handle);

vfs_open_handle_t *vfs_handle_get(u32 handle);

#endif /* _VFS_HANDLE_H_ */
