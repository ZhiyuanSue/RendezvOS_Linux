#ifndef _VFS_BACKEND_OPS_H_
#define _VFS_BACKEND_OPS_H_

#include <common/stdbool.h>
#include <common/types.h>

#include <linux_compat/fs/vfs_path.h>

/*
 * Middle-layer inode handle. Path resolution lives in vfs_namespace.c.
 * I/O goes to backend_port via vfs_backend_dispatch(); VFS does not care
 * which concrete backend owns that port.
 */

typedef struct vfs_inode {
        char path[VFS_PATH_MAX];
        const char *backend_port;
        u32 backend_caps;
        void *storage;
        u32 mode;
        u64 size;
        u32 nlink;
        bool is_dir;
        bool writable;
} vfs_inode_t;

void vfs_inode_init_synthetic_root(vfs_inode_t *out);

i64 vfs_inode_read(const vfs_inode_t *ino, u64 offset, void *buf, u64 len);
i64 vfs_inode_write(vfs_inode_t *ino, u64 offset, const void *buf, u64 len);
i64 vfs_inode_truncate(vfs_inode_t *ino, u64 size);
i64 vfs_inode_flush_backing(const vfs_inode_t *ino);

#endif /* _VFS_BACKEND_OPS_H_ */
