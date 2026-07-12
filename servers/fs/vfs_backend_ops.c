/*
 * Per-backend lookup/read/write/truncate for vfs_inode handles.
 * Thin dispatch into vfs_backend message framework.
 */

#include "vfs_backend_ops.h"

#include "vfs_backend.h"

#include <common/string.h>
#include <linux_compat/errno.h>

#define VFS_S_IFDIR 0040000u

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

static i64 vfs_backend_msg_result(const char *port, vfs_backend_op_t op,
                                  const char *path, vfs_inode_t *ino,
                                  vfs_inode_t *ino_out, u64 offset, u64 len,
                                  void *buf, const void *wbuf, u64 size_arg)
{
        vfs_backend_req_t req;

        req.port = port;
        req.op = op;
        req.path = path;
        req.ino = ino;
        req.ino_out = ino_out;
        req.offset = offset;
        req.len = len;
        req.buf = buf;
        req.wbuf = wbuf;
        req.size_arg = size_arg;
        req.result = -LINUX_EINVAL;

        return vfs_backend_dispatch(&req);
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

i64 vfs_inode_read(const vfs_inode_t *ino, u64 offset, void *buf, u64 len)
{
        if (!ino || !buf) {
                return vfs_err_inval();
        }
        if (ino->is_dir) {
                return vfs_err_isdir();
        }
        if (!ino->backend_port) {
                return 0;
        }

        return vfs_backend_msg_result(ino->backend_port, VFS_BACKEND_OP_READ,
                                      NULL, (vfs_inode_t *)ino, NULL, offset,
                                      len, buf, NULL, 0);
}

i64 vfs_inode_write(vfs_inode_t *ino, u64 offset, const void *buf, u64 len)
{
        u32 caps;

        if (!ino || !buf) {
                return vfs_err_inval();
        }
        if (ino->is_dir) {
                return vfs_err_isdir();
        }
        if (!ino->backend_port) {
                return vfs_err_rofs();
        }

        caps = ino->backend_caps;
        if (!(caps & VFS_BACKEND_CAP_WRITE_SOURCE)
            && !(caps & VFS_BACKEND_CAP_WRITE_CACHE)) {
                return vfs_err_rofs();
        }

        return vfs_backend_msg_result(ino->backend_port, VFS_BACKEND_OP_WRITE,
                                      NULL, ino, NULL, offset, len, NULL, buf,
                                      0);
}

i64 vfs_inode_truncate(vfs_inode_t *ino, u64 size)
{
        if (!ino) {
                return vfs_err_inval();
        }
        if (ino->is_dir) {
                return vfs_err_isdir();
        }
        if (!ino->backend_port
            || !(ino->backend_caps & VFS_BACKEND_CAP_WRITE_SOURCE)) {
                return vfs_err_rofs();
        }

        return vfs_backend_msg_result(ino->backend_port, VFS_BACKEND_OP_TRUNCATE,
                                      NULL, ino, NULL, 0, 0, NULL, NULL, size);
}

i64 vfs_inode_flush_backing(const vfs_inode_t *ino)
{
        u32 caps;

        if (!ino || !ino->backend_port) {
                return vfs_err_inval();
        }

        caps = ino->backend_caps;
        if (caps & VFS_BACKEND_CAP_FLUSH_DROP) {
                return vfs_backend_msg_result(ino->backend_port,
                                              VFS_BACKEND_OP_FLUSH, NULL,
                                              (vfs_inode_t *)ino, NULL, 0, 0,
                                              NULL, NULL, 0);
        }
        if (caps & VFS_BACKEND_CAP_WRITE_SOURCE) {
                return vfs_backend_msg_result(ino->backend_port,
                                              VFS_BACKEND_OP_FLUSH, NULL,
                                              (vfs_inode_t *)ino, NULL, 0, 0,
                                              NULL, NULL, 0);
        }

        return 0;
}
