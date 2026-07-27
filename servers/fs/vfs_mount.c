#include "vfs_mount.h"

#include "vfs_backend.h"
#include "vfs_handle.h"
#include "vfs_namespace.h"
#include "vfs_slice_table.h"
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

static vfs_slice_table_t vfs_mount_tab;

static vfs_mount_rec_t *vfs_mount_at(u32 i)
{
        return (vfs_mount_rec_t *)vfs_slice_table_ptr(&vfs_mount_tab, i);
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
        vfs_slice_table_destroy(&vfs_mount_tab);
        if (vfs_slice_table_init(&vfs_mount_tab, sizeof(vfs_mount_rec_t), 8)
            == REND_SUCCESS) {
                vfs_mount_tab.soft_max = VFS_MOUNT_SOFT_MAX;
        }
}

const char *vfs_mount_backend_port_for_path(const char *path)
{
        vfs_mount_view_t view;

        if (!vfs_mount_view_for_path(path, &view)) {
                return NULL;
        }

        return view.backend_port;
}

bool vfs_mount_view_for_path(const char *path, vfs_mount_view_t *out)
{
        u32 i;
        u32 n;
        char norm[VFS_PATH_MAX];
        u64 best_len = 0;
        vfs_mount_rec_t *best = NULL;

        if (!path) {
                return false;
        }

        vfs_path_normalize(path, norm, sizeof(norm));
        n = vfs_slice_table_count(&vfs_mount_tab);

        for (i = 0; i < n; i++) {
                vfs_mount_rec_t *m = vfs_mount_at(i);
                u64 mlen;

                if (!m || !m->active || !m->backend_port[0]) {
                        continue;
                }
                if (!vfs_mount_path_under_or_equal(norm, m->target)) {
                        continue;
                }

                mlen = strlen(m->target);
                if (mlen > best_len) {
                        best_len = mlen;
                        best = m;
                }
        }

        if (!best) {
                return false;
        }

        if (out) {
                out->target = best->target;
                out->backend_port = best->backend_port;
                out->fstype = best->fstype;
                out->flags = best->flags;
        }
        return true;
}

i64 vfs_mount_register(const char *target, const char *fstype, u64 flags)
{
        u32 i;
        u32 n;
        u32 idx;
        char norm[VFS_PATH_MAX];
        const char *backend_port;
        vfs_mount_rec_t *slot;
        error_t err;

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

        n = vfs_slice_table_count(&vfs_mount_tab);
        for (i = 0; i < n; i++) {
                vfs_mount_rec_t *m = vfs_mount_at(i);

                if (m && m->active && vfs_path_equal(m->target, norm)) {
                        return 0;
                }
        }

        slot = NULL;
        for (i = 0; i < n; i++) {
                vfs_mount_rec_t *m = vfs_mount_at(i);

                if (m && !m->active) {
                        slot = m;
                        break;
                }
        }
        if (!slot) {
                err = vfs_slice_table_push_zero(&vfs_mount_tab, &idx);
                if (err != REND_SUCCESS) {
                        return -LINUX_ENOMEM;
                }
                slot = vfs_mount_at(idx);
                if (!slot) {
                        vfs_slice_table_pop_last(&vfs_mount_tab);
                        return -LINUX_ENOMEM;
                }
        }

        memset(slot, 0, sizeof(*slot));
        strncpy(slot->target, norm, sizeof(slot->target) - 1);
        strncpy(slot->fstype, fstype, sizeof(slot->fstype) - 1);
        strncpy(slot->backend_port, backend_port, sizeof(slot->backend_port) - 1);
        slot->flags = flags;
        slot->active = true;

        if (strcmp_s(fstype, VFS_BACKEND_FSTYPE_RAMFS, VFS_BACKEND_FSTYPE_MAX)
            == 0) {
                (void)vfs_backend_mkdir(backend_port, norm, 0755u | 0040000u);
        }
        (void)vfs_namespace_set_mount_cover(norm, true);
        return 0;
}

i64 vfs_mount_unregister(const char *target, u64 flags)
{
        u32 i;
        u32 n;
        char norm[VFS_PATH_MAX];

        if (!target) {
                return -LINUX_EINVAL;
        }

        vfs_path_normalize(target, norm, sizeof(norm));
        n = vfs_slice_table_count(&vfs_mount_tab);

        for (i = 0; i < n; i++) {
                vfs_mount_rec_t *m = vfs_mount_at(i);

                if (m && m->active && vfs_path_equal(m->target, norm)) {
                        if ((flags & LINUX_MNT_DETACH) == 0
                            && vfs_handle_busy_under_path(norm)) {
                                return -LINUX_EBUSY;
                        }

                        (void)vfs_namespace_set_mount_cover(norm, false);
                        memset(m, 0, sizeof(*m));
                        return 0;
                }
        }

        return -LINUX_EINVAL;
}

bool vfs_mount_is_mountpoint(const char *path)
{
        u32 i;
        u32 n;

        if (!path) {
                return false;
        }

        n = vfs_slice_table_count(&vfs_mount_tab);
        for (i = 0; i < n; i++) {
                vfs_mount_rec_t *m = vfs_mount_at(i);

                if (m && m->active && vfs_path_equal(m->target, path)) {
                        return true;
                }
        }

        return false;
}

i64 vfs_mount_apply_cover(const char *target, bool covered)
{
        return vfs_namespace_set_mount_cover(target, covered);
}
