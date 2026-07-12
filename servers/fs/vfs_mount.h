#ifndef _VFS_MOUNT_H_
#define _VFS_MOUNT_H_

#include <common/stdbool.h>
#include <common/types.h>

#define VFS_MOUNT_MAX 8u
#define VFS_MOUNT_PORT_NAME_MAX 32u

typedef struct vfs_mount_view {
        const char *target;
        const char *backend_port;
        const char *fstype;
        u64 flags;
} vfs_mount_view_t;

void vfs_mount_reset(void);

i64 vfs_mount_register(const char *target, const char *fstype, u64 flags);
i64 vfs_mount_unregister(const char *target, u64 flags);

bool vfs_mount_is_mountpoint(const char *path);

/*
 * Longest-prefix mount match: if @path lies under an active mount, fill @out
 * and return true.
 */
bool vfs_mount_view_for_path(const char *path, vfs_mount_view_t *out);

const char *vfs_mount_backend_port_for_path(const char *path);

i64 vfs_mount_apply_cover(const char *target, bool covered);

#endif /* _VFS_MOUNT_H_ */
