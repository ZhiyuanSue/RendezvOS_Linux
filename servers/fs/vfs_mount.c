#include "vfs_mount.h"

#include "vfs_backend.h"
#include "vfs_handle.h"
#include "vfs_namespace.h"
#include <linux_compat/fs/vfs_path.h>

#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/linux_mount.h>

typedef struct vfs_mount_rec {
        char target[VFS_PATH_MAX];
        char fstype[16];
        char backend_port[VFS_MOUNT_PORT_NAME_MAX];
        u64 flags;
        bool active;
} vfs_mount_rec_t;

static vfs_mount_rec_t vfs_mounts[VFS_MOUNT_MAX];

static bool vfs_mount_path_equal(const char *a, const char *b)
{
        return vfs_path_equal(a, b);
}

static bool vfs_mount_path_under_or_equal(const char *path, const char *mount)
{
        char norm_path[VFS_PATH_MAX];
        char norm_mount[VFS_PATH_MAX];
        u64 mlen;

        if (!path || !mount) {
                return false;
        }

        vfs_path_normalize(path, norm_path, sizeof(norm_path));
        vfs_path_normalize(mount, norm_mount, sizeof(norm_mount));

        if (vfs_path_equal(norm_path, norm_mount)) {
                return true;
        }

        mlen = strlen(norm_mount);
        if (mlen <= 1) {
                return norm_path[0] == '/' && norm_path[1] != '\0';
        }

        if (strcmp_s(norm_path, norm_mount, mlen) != 0) {
                return false;
        }

        return norm_path[mlen] == '/';
}

void vfs_mount_reset(void)
{
        memset(vfs_mounts, 0, sizeof(vfs_mounts));
}

const char *vfs_mount_backend_port_for_path(const char *path)
{
        u32 i;
        const char *best = NULL;
        u64 best_len = 0;
        char norm[VFS_PATH_MAX];

        if (!path) {
                return NULL;
        }

        vfs_path_normalize(path, norm, sizeof(norm));

        for (i = 0; i < VFS_MOUNT_MAX; i++) {
                u64 mlen;

                if (!vfs_mounts[i].active || !vfs_mounts[i].backend_port[0]) {
                        continue;
                }
                if (!vfs_mount_path_under_or_equal(norm, vfs_mounts[i].target)) {
                        continue;
                }

                mlen = strlen(vfs_mounts[i].target);
                if (mlen > best_len) {
                        best_len = mlen;
                        best = vfs_mounts[i].backend_port;
                }
        }

        return best;
}

i64 vfs_mount_register(const char *target, const char *fstype, u64 flags)
{
        u32 i;
        char norm[VFS_PATH_MAX];
        const char *backend_port;

        if (!target || !fstype) {
                return -LINUX_EINVAL;
        }

        backend_port = vfs_backend_port_for_fstype(fstype);
        if (!backend_port) {
                return -LINUX_ENODEV;
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
                        strncpy(vfs_mounts[i].backend_port, backend_port,
                                sizeof(vfs_mounts[i].backend_port) - 1);
                        vfs_mounts[i].backend_port
                                [sizeof(vfs_mounts[i].backend_port) - 1] = '\0';
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
                        vfs_mounts[i].backend_port[0] = '\0';
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
