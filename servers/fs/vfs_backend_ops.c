/*
 * Middle-layer inode helpers (path resolution lives in vfs_namespace).
 * Listen I/O goes through coop nested backend RPC, not these wrappers.
 */

#include "vfs_backend_ops.h"

#include <common/string.h>

#define VFS_S_IFDIR 0040000u

void vfs_inode_fill_path(vfs_inode_t *ino, const char *path)
{
        if (!ino || !path) {
                return;
        }
        strncpy(ino->path, path, sizeof(ino->path) - 1);
        ino->path[sizeof(ino->path) - 1] = '\0';
}

void vfs_inode_init_synthetic_root(vfs_inode_t *out)
{
        if (!out) {
                return;
        }

        memset(out, 0, sizeof(*out));
        out->path[0] = '/';
        out->path[1] = '\0';
        out->backend_port = NULL;
        out->backend_caps = 0;
        out->mode = VFS_S_IFDIR | 0755u;
        out->nlink = 2;
        out->is_dir = true;
        out->writable = false;
}
