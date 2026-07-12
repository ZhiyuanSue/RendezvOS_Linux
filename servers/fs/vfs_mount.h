#ifndef _VFS_MOUNT_H_
#define _VFS_MOUNT_H_

#include <common/stdbool.h>
#include <common/types.h>

#define VFS_MOUNT_MAX 8u
#define VFS_MOUNT_PORT_NAME_MAX 32u

void vfs_mount_reset(void);

i64 vfs_mount_register(const char *target, const char *fstype, u64 flags);
i64 vfs_mount_unregister(const char *target, u64 flags);

bool vfs_mount_is_mountpoint(const char *path);

/*
 * Longest-prefix mount match: if @path lies under an active mount, return that
 * mount's registered backend port; otherwise NULL.
 */
const char *vfs_mount_backend_port_for_path(const char *path);

/* Hide cpio-only children under @target (P3-2 bootstrap overlay). */
i64 vfs_mount_apply_cover(const char *target, bool covered);

#endif /* _VFS_MOUNT_H_ */
