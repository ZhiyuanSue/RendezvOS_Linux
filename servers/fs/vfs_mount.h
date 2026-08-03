#ifndef _VFS_MOUNT_H_
#define _VFS_MOUNT_H_

#include <common/stdbool.h>
#include <common/types.h>

#define VFS_MOUNT_SOFT_MAX      64u
#define VFS_MOUNT_PORT_NAME_MAX 32u

/* Mount table is growable (vfs_slice_table); soft max VFS_MOUNT_SOFT_MAX. */

typedef struct vfs_mount_view {
        const char *target;
        const char *backend_port;
        const char *fstype;
        u64 flags;
} vfs_mount_view_t;

void vfs_mount_reset(void);

/*
 * Insert/activate mount record.
 *   0 — already mounted (done)
 *   1 — registered; if *@need_mkdir, park nested MKDIR then cover, else cover
 *  <0 — errno
 */
i64 vfs_mount_register_prepare(const char *target, const char *fstype,
                               u64 flags, const char **port_out,
                               char *norm_out, u64 norm_cap, bool *need_mkdir);

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
