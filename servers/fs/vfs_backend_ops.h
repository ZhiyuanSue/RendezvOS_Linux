#ifndef _VFS_BACKEND_OPS_H_
#define _VFS_BACKEND_OPS_H_

#include <common/stdbool.h>
#include <common/types.h>

#include "ramfs_layer.h"
#include "vfs_path.h"

/*
 * Middle-layer inode handle + per-backend I/O dispatch (Step 1).
 * Overlay path resolution stays in vfs_root.c; backends only touch their layer.
 */

typedef enum vfs_backend {
        VFS_BACKEND_NONE = 0,
        VFS_BACKEND_CPIO,
        VFS_BACKEND_RAMFS,
} vfs_backend_t;

typedef struct vfs_inode {
        char path[VFS_PATH_MAX];
        vfs_backend_t backend;
        union {
                const u8 *cpio_data;
                ramfs_entry_t *ram;
        } u;
        u32 mode;
        u64 size;
        u32 nlink;
        bool is_dir;
        bool writable;
} vfs_inode_t;

/* Legacy name used in architecture docs; same layout. */
typedef vfs_inode_t vfs_object_t;

bool vfs_backend_cpio_lookup(const char *path, vfs_inode_t *out);
bool vfs_backend_ramfs_lookup(const char *path, vfs_inode_t *out);
void vfs_inode_init_synthetic_root(vfs_inode_t *out);

i64 vfs_inode_read(const vfs_inode_t *ino, u64 offset, void *buf, u64 len);
i64 vfs_inode_write(vfs_inode_t *ino, u64 offset, const void *buf, u64 len);
i64 vfs_inode_truncate(vfs_inode_t *ino, u64 size);

#endif /* _VFS_BACKEND_OPS_H_ */
