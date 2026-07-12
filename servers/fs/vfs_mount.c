#include "vfs_mount.h"

#include "vfs_handle.h"
#include "vfs_namespace.h"
#include <linux_compat/fs/vfs_path.h>

#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/linux_mount.h>

typedef struct vfs_mount_rec {
        char target[VFS_PATH_MAX];
        char fstype[16];
        u64 flags;
        bool active;
} vfs_mount_rec_t;

static vfs_mount_rec_t vfs_mounts[VFS_MOUNT_MAX];

static bool vfs_mount_path_equal(const char *a, const char *b)
{
        char na[VFS_PATH_MAX];
        char nb[VFS_PATH_MAX];

        vfs_path_normalize(a, na, sizeof(na));
        vfs_path_normalize(b, nb, sizeof(nb));
        return strcmp_s(na, nb, VFS_PATH_MAX) == 0;
}

void vfs_mount_reset(void)
{
        memset(vfs_mounts, 0, sizeof(vfs_mounts));
}

i64 vfs_mount_register(const char *target, const char *fstype, u64 flags)
{
        u32 i;
        char norm[VFS_PATH_MAX];

        if (!target || !fstype) {
                return -LINUX_EINVAL;
        }

        vfs_path_normalize(target, norm, sizeof(norm));
        if (!vfs_path_is_root(norm) && norm[0] == '\0') {
                return -LINUX_EINVAL;
        }

        for (i = 0; i < VFS_MOUNT_MAX; i++) {
                if (vfs_mounts[i].active
                    && vfs_mount_path_equal(vfs_mounts[i].target, norm)) {
                        return 0;
                }
        }

        for (i = 0; i < VFS_MOUNT_MAX; i++) {
                if (!vfs_mounts[i].active) {
                        strncpy(vfs_mounts[i].target, norm,
                                sizeof(vfs_mounts[i].target) - 1);
                        vfs_mounts[i].target[sizeof(vfs_mounts[i].target) - 1] =
                                '\0';
                        strncpy(vfs_mounts[i].fstype, fstype,
                                sizeof(vfs_mounts[i].fstype) - 1);
                        vfs_mounts[i].fstype[sizeof(vfs_mounts[i].fstype) - 1] =
                                '\0';
                        vfs_mounts[i].flags = flags;
                        vfs_mounts[i].active = true;
                        (void)vfs_namespace_set_mount_cover(norm, true);
                        return 0;
                }
        }

        return -LINUX_ENOMEM;
}

i64 vfs_mount_unregister(const char *target, u64 flags)
{
        u32 i;
        char norm[VFS_PATH_MAX];

        if (!target) {
                return -LINUX_EINVAL;
        }

        vfs_path_normalize(target, norm, sizeof(norm));

        for (i = 0; i < VFS_MOUNT_MAX; i++) {
                if (vfs_mounts[i].active
                    && vfs_mount_path_equal(vfs_mounts[i].target, norm)) {
                        if ((flags & LINUX_MNT_DETACH) == 0
                            && vfs_handle_busy_under_path(norm)) {
                                return -LINUX_EBUSY;
                        }

                        (void)vfs_namespace_set_mount_cover(norm, false);
                        vfs_mounts[i].active = false;
                        vfs_mounts[i].target[0] = '\0';
                        vfs_mounts[i].fstype[0] = '\0';
                        vfs_mounts[i].flags = 0;
                        return 0;
                }
        }

        return -LINUX_EINVAL;
}

bool vfs_mount_is_mountpoint(const char *path)
{
        u32 i;

        if (!path) {
                return false;
        }

        for (i = 0; i < VFS_MOUNT_MAX; i++) {
                if (vfs_mounts[i].active
                    && vfs_mount_path_equal(vfs_mounts[i].target, path)) {
                        return true;
                }
        }

        return false;
}

i64 vfs_mount_apply_cover(const char *target, bool covered)
{
        return vfs_namespace_set_mount_cover(target, covered);
}
