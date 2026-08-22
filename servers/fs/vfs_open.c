/*
 * VFS path/handle front-end (scheme B).
 *
 * Listen-side opcodes are handled by vfs_coop*.c. This file keeps: kern
 * lookup, open_install, and local handle helpers (lseek/fstat).
 */

#include "vfs_open.h"

#include "vfs_handle.h"
#include <linux_compat/fs/vfs_path.h>
#include "vfs_namespace.h"
#include "vfs_rpc.h"

#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/linux_mm_radix.h>
#include <rendezvos/mm/vmm.h>

bool vfs_inode_symlink_target(const vfs_inode_t *ino, char *out, u64 cap)
{
        u64 len;

        if (!ino || !ino->is_symlink || !ino->storage || !out || cap == 0) {
                return false;
        }

        len = ino->size;
        if (len >= cap) {
                len = cap - 1;
        }
        memcpy(out, ino->storage, (size_t)len);
        out[len] = '\0';
        return true;
}

void vfs_join_symlink_target(const char *base, const char *target, char *out,
                             u64 cap)
{
        char parent[VFS_PATH_MAX];

        if (!target || !out || cap == 0) {
                return;
        }

        if (target[0] == '/') {
                vfs_path_normalize(target, out, cap);
                return;
        }

        if (!vfs_path_parent(base, parent, sizeof(parent))) {
                (void)vfs_path_join("/", target, out, cap);
                return;
        }

        (void)vfs_path_join(parent, target, out, cap);
}

i64 vfs_lookup_path(const char *path, vfs_inode_t *out, bool follow_symlink)
{
        char target[VFS_PATH_MAX];
        char resolved[VFS_PATH_MAX];
        i64 ret;

        if (!path || !out) {
                return -LINUX_EINVAL;
        }

        ret = vfs_namespace_lookup(path, out);
        if (ret < 0) {
                return ret;
        }
        if (!follow_symlink || !out->is_symlink) {
                return 0;
        }
        if (!vfs_inode_symlink_target(out, target, sizeof(target))) {
                return -LINUX_EINVAL;
        }

        vfs_join_symlink_target(path, target, resolved, sizeof(resolved));
        if (resolved[0] == '\0') {
                return -LINUX_EINVAL;
        }

        return vfs_namespace_lookup(resolved, out);
}

i64 vfs_open_install(const vfs_inode_t *ino, i32 flags)
{
        i32 acc = flags & VFS_O_ACCMODE;
        u32 handle;

        if (!ino) {
                return -LINUX_EINVAL;
        }

        if ((flags & VFS_O_DIRECTORY) && !ino->is_dir) {
                return -LINUX_ENOTDIR;
        }

        if (!(flags & VFS_O_DIRECTORY) && ino->is_dir && acc != VFS_O_RDONLY) {
                return -LINUX_EISDIR;
        }

        if (ino->is_dir && acc != VFS_O_RDONLY) {
                if (acc == VFS_O_WRONLY || acc == VFS_O_RDWR) {
                        return -LINUX_EISDIR;
                }
        }

        if (!ino->is_dir && acc != VFS_O_RDONLY) {
                if (!ino->writable) {
                        return -LINUX_EACCES;
                }
        }

        handle = vfs_handle_open(ino, flags);
        if (handle == 0) {
                return -LINUX_EMFILE;
        }

        return (i64)handle | (ino->is_dir ? VFS_OPEN_RET_IS_DIR_BIT : 0);
}

i64 vfs_lseek_handle(u32 handle, i64 offset, i32 whence)
{
        vfs_open_handle_t *file;
        i64 new_off;

        file = vfs_handle_get(handle);
        if (!file) {
                return -LINUX_EBADF;
        }

        if (file->ino.is_dir) {
                return -LINUX_EISDIR;
        }

        switch (whence) {
        case 0:
                new_off = offset;
                break;
        case 1:
                new_off = (i64)file->offset + offset;
                break;
        case 2:
                new_off = (i64)file->ino.size + offset;
                break;
        default:
                return -LINUX_EINVAL;
        }

        if (new_off < 0) {
                return -LINUX_EINVAL;
        }

        file->offset = (u64)new_off;
        return new_off;
}

i64 vfs_fstat_handle(pid_t pid, u32 handle, u64 user_statbuf)
{
        linux_proc_resource_t *task = vfs_task_user_for_pid(pid);
        vfs_open_handle_t *file;

        if (!task) {
                return -LINUX_ESRCH;
        }

        file = vfs_handle_get(handle);
        if (!file) {
                return -LINUX_EBADF;
        }

        return vfs_store_inode_stat(task, user_statbuf, &file->ino);
}
