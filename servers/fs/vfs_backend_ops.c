/*
 * Per-backend lookup/read/write/truncate for vfs_inode handles.
 */

#include "vfs_backend_ops.h"

#include "cpio_rofs.h"
#include "vfs_page_cache.h"

#include <common/string.h>
#include <linux_compat/errno.h>

typedef struct vfs_backend_ops {
        vfs_backend_t id;
        bool (*lookup)(const char *path, vfs_inode_t *out);
        i64 (*read)(const vfs_inode_t *ino, u64 offset, void *buf, u64 len);
        i64 (*write)(vfs_inode_t *ino, u64 offset, const void *buf, u64 len);
        i64 (*truncate)(vfs_inode_t *ino, u64 size);
} vfs_backend_ops_t;

static i64 vfs_err_rofs(void)
{
        return -LINUX_EROFS;
}

static i64 vfs_err_isdir(void)
{
        return -LINUX_EISDIR;
}

static i64 vfs_err_inval(void)
{
        return -LINUX_EINVAL;
}

static i64 vfs_err_nomem(void)
{
        return -LINUX_ENOMEM;
}

static void vfs_inode_fill_path(vfs_inode_t *ino, const char *path)
{
        if (!ino || !path) {
                return;
        }
        strncpy(ino->path, path, sizeof(ino->path) - 1);
        ino->path[sizeof(ino->path) - 1] = '\0';
}

static bool vfs_backend_cpio_lookup_impl(const char *path, vfs_inode_t *out)
{
        cpio_rofs_stat_t st;

        if (!path || !out || !cpio_rofs_lookup(path, &st)) {
                return false;
        }

        memset(out, 0, sizeof(*out));
        vfs_inode_fill_path(out, path);
        out->backend = VFS_BACKEND_CPIO;
        out->u.cpio_data = st.data;
        out->mode = st.mode;
        out->size = st.size;
        out->nlink = st.nlink ? st.nlink : 1u;
        out->is_dir = st.is_dir;
        out->writable = false;
        return true;
}

static bool vfs_backend_ramfs_lookup_impl(const char *path, vfs_inode_t *out)
{
        const ramfs_entry_t *ent;

        if (!path || !out) {
                return false;
        }

        ent = ramfs_lookup(path);
        if (!ent) {
                return false;
        }

        memset(out, 0, sizeof(*out));
        vfs_inode_fill_path(out, path);
        out->backend = VFS_BACKEND_RAMFS;
        out->u.ram = (ramfs_entry_t *)ent;
        out->mode = ent->mode;
        out->size = ent->size;
        out->nlink = 1;
        out->is_dir = (ent->flags & RAMFS_FLAG_DIR) != 0;
        out->writable = true;
        return true;
}

static i64 vfs_backend_cpio_read(const vfs_inode_t *ino, u64 offset, void *buf,
                                 u64 len)
{
        cpio_rofs_stat_t st;
        i64 ret;

        if (!ino || !buf) {
                return vfs_err_inval();
        }
        if (ino->is_dir) {
                return vfs_err_isdir();
        }

        st.mode = ino->mode;
        st.size = ino->size;
        st.is_dir = ino->is_dir;
        st.data = ino->u.cpio_data;
        ret = vfs_page_cache_read_cpio(ino->path, st.data, st.size, offset, buf,
                                       len);
        if (ret >= 0) {
                return ret;
        }
        ret = cpio_rofs_read(&st, offset, buf, len);
        if (ret < 0) {
                return vfs_err_inval();
        }
        return ret;
}

static i64 vfs_backend_ramfs_read(const vfs_inode_t *ino, u64 offset, void *buf,
                                  u64 len)
{
        i64 ret;

        if (!ino || !buf || !ino->u.ram) {
                return vfs_err_inval();
        }
        if (ino->is_dir) {
                return vfs_err_isdir();
        }

        ret = ramfs_read(ino->u.ram, offset, buf, len);
        if (ret < 0) {
                return vfs_err_inval();
        }
        return ret;
}

static i64 vfs_backend_ramfs_write(vfs_inode_t *ino, u64 offset,
                                   const void *buf, u64 len)
{
        i64 ret;

        if (!ino || !buf || !ino->u.ram) {
                return vfs_err_inval();
        }
        if (ino->is_dir) {
                return vfs_err_isdir();
        }

        ret = ramfs_write(ino->u.ram, offset, buf, len);
        if (ret < 0) {
                if (ret == (i64)-E_IN_PARAM) {
                        return vfs_err_inval();
                }
                return vfs_err_nomem();
        }

        vfs_page_cache_drop(ino->path);
        ino->size = ino->u.ram->size;
        return ret;
}

static i64 vfs_backend_ramfs_truncate(vfs_inode_t *ino, u64 size)
{
        error_t err;

        if (!ino || !ino->u.ram) {
                return vfs_err_inval();
        }
        if (ino->is_dir) {
                return vfs_err_isdir();
        }

        err = ramfs_truncate(ino->u.ram, size);
        if (err != REND_SUCCESS) {
                return vfs_err_nomem();
        }

        vfs_page_cache_drop(ino->path);
        ino->size = ino->u.ram->size;
        return 0;
}

static const vfs_backend_ops_t vfs_cpio_ops = {
        .id = VFS_BACKEND_CPIO,
        .lookup = vfs_backend_cpio_lookup_impl,
        .read = vfs_backend_cpio_read,
        .write = NULL,
        .truncate = NULL,
};

static const vfs_backend_ops_t vfs_ramfs_ops = {
        .id = VFS_BACKEND_RAMFS,
        .lookup = vfs_backend_ramfs_lookup_impl,
        .read = vfs_backend_ramfs_read,
        .write = vfs_backend_ramfs_write,
        .truncate = vfs_backend_ramfs_truncate,
};

static const vfs_backend_ops_t vfs_none_ops = {
        .id = VFS_BACKEND_NONE,
        .lookup = NULL,
        .read = NULL,
        .write = NULL,
        .truncate = NULL,
};

static const vfs_backend_ops_t *vfs_backend_ops(vfs_backend_t backend)
{
        switch (backend) {
        case VFS_BACKEND_CPIO:
                return &vfs_cpio_ops;
        case VFS_BACKEND_RAMFS:
                return &vfs_ramfs_ops;
        default:
                return &vfs_none_ops;
        }
}

bool vfs_backend_cpio_lookup(const char *path, vfs_inode_t *out)
{
        return vfs_backend_cpio_lookup_impl(path, out);
}

bool vfs_backend_ramfs_lookup(const char *path, vfs_inode_t *out)
{
        return vfs_backend_ramfs_lookup_impl(path, out);
}

void vfs_inode_init_synthetic_root(vfs_inode_t *out)
{
        if (!out) {
                return;
        }

        memset(out, 0, sizeof(*out));
        out->path[0] = '/';
        out->path[1] = '\0';
        out->backend = VFS_BACKEND_NONE;
        out->mode = RAMFS_S_IFDIR | 0755u;
        out->nlink = 2;
        out->is_dir = true;
        out->writable = false;
}

i64 vfs_inode_read(const vfs_inode_t *ino, u64 offset, void *buf, u64 len)
{
        const vfs_backend_ops_t *ops;

        if (!ino || !buf) {
                return vfs_err_inval();
        }
        if (ino->is_dir) {
                return vfs_err_isdir();
        }

        ops = vfs_backend_ops(ino->backend);
        if (!ops || !ops->read) {
                return 0;
        }
        return ops->read(ino, offset, buf, len);
}

i64 vfs_inode_write(vfs_inode_t *ino, u64 offset, const void *buf, u64 len)
{
        const vfs_backend_ops_t *ops;

        if (!ino || !buf) {
                return vfs_err_inval();
        }
        if (ino->backend == VFS_BACKEND_CPIO) {
                return vfs_err_rofs();
        }

        ops = vfs_backend_ops(ino->backend);
        if (!ops || !ops->write) {
                return vfs_err_inval();
        }
        return ops->write(ino, offset, buf, len);
}

i64 vfs_inode_truncate(vfs_inode_t *ino, u64 size)
{
        const vfs_backend_ops_t *ops;

        if (!ino) {
                return vfs_err_inval();
        }
        if (ino->backend == VFS_BACKEND_CPIO) {
                return vfs_err_rofs();
        }

        ops = vfs_backend_ops(ino->backend);
        if (!ops || !ops->truncate) {
                return vfs_err_inval();
        }
        return ops->truncate(ino, size);
}
