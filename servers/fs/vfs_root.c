/*
 * VFS root mount: init + thin facade over vfs_namespace (middle layer).
 */

#include "cpio_rofs.h"
#include "vfs_backend_ops.h"
#include "vfs_kstat.h"
#include "vfs_namespace.h"
#include "vfs_root.h"

#include <linux_compat/errno.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>

static bool vfs_root_initialized;

error_t vfs_root_init(const void *cpio_image, u64 cpio_len)
{
        error_t err;

        if (vfs_root_initialized) {
                return REND_SUCCESS;
        }
        if (!cpio_image || cpio_len == 0) {
                return -E_IN_PARAM;
        }

        vfs_namespace_reset();

        err = cpio_rofs_init(cpio_image, cpio_len);
        if (err != REND_SUCCESS) {
                return err;
        }

        err = vfs_namespace_init();
        if (err != REND_SUCCESS) {
                return err;
        }

        vfs_root_initialized = true;

        pr_info("[VFS] root ready: cpio %u entries, namespace %u entries\n",
                cpio_rofs_parsed_count(),
                vfs_namespace_count());
        return REND_SUCCESS;
}

error_t vfs_root_ensure_init(const void *cpio_image, u64 cpio_len)
{
        return vfs_root_init(cpio_image, cpio_len);
}

i64 vfs_root_lookup(const char *path, vfs_inode_t *out)
{
        return vfs_namespace_lookup(path, out);
}

i64 vfs_root_mkdir(const char *path, u32 mode)
{
        return vfs_namespace_mkdir(path, mode);
}

i64 vfs_root_create_file(const char *path, u32 mode, vfs_inode_t *out)
{
        return vfs_namespace_create_file(path, mode, out);
}

i64 vfs_root_unlink(const char *path)
{
        return vfs_namespace_unlink(path);
}

i64 vfs_root_rename(const char *oldpath, const char *newpath)
{
        return vfs_namespace_rename(oldpath, newpath);
}

i64 vfs_root_link(const char *oldpath, const char *newpath)
{
        return vfs_namespace_link(oldpath, newpath);
}

i64 vfs_root_read(const vfs_inode_t *ino, u64 offset, void *buf, u64 len)
{
        return vfs_inode_read(ino, offset, buf, len);
}

i64 vfs_root_write(vfs_inode_t *ino, u64 offset, const void *buf, u64 len)
{
        return vfs_inode_write(ino, offset, buf, len);
}

i64 vfs_root_truncate(vfs_inode_t *ino, u64 size)
{
        return vfs_inode_truncate(ino, size);
}

i64 vfs_root_stat(const char *path, vfs_kstat_t *out)
{
        vfs_inode_t ino;
        i64 ret;

        if (!path || !out) {
                return -LINUX_EINVAL;
        }

        ret = vfs_namespace_lookup(path, &ino);
        if (ret < 0) {
                return ret;
        }

        vfs_kstat_from_inode(&ino, out);
        return 0;
}

i64 vfs_root_readdir(const char *dirpath, u64 index, vfs_dirent_t *out)
{
        return vfs_namespace_readdir(dirpath, index, out);
}
