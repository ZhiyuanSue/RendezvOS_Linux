/*
 * VFS server open/read front-end — path + handle API (scheme B).
 */

#include "vfs_open.h"

#include "vfs_handle.h"
#include <linux_compat/fs/vfs_path.h>
#include "vfs_root.h"

#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_registry.h>
#include <rendezvos/mm/vmm.h>

#define VFS_READ_CHUNK 4096u
#define VFS_S_IFREG    0100000u

static Tcb_Base *vfs_task_for_pid(pid_t pid)
{
        Tcb_Base *task = find_task_by_pid(pid);

        if (!task || !task->vs || !linux_vspace_is_user_table(task->vs)) {
                return NULL;
        }

        return task;
}

static i64 vfs_store_kstat(Tcb_Base *task, u64 user_statbuf,
                           const vfs_kstat_t *st)
{
        error_t e;

        if (!task || !st) {
                return -LINUX_EINVAL;
        }

        e = linux_mm_store_to_user(task->vs, user_statbuf, st, sizeof(*st));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        return 0;
}

i64 vfs_open_path(const char *path, i32 flags, u32 mode)
{
        vfs_inode_t ino;
        i64 ret;
        i32 acc = flags & VFS_O_ACCMODE;
        u32 handle;

        if (!path) {
                return -LINUX_EINVAL;
        }

        ret = vfs_root_lookup(path, &ino);

        if (ret < 0) {
                if (!(flags & VFS_O_CREAT)) {
                        return ret;
                }

                if (flags & VFS_O_DIRECTORY) {
                        ret = vfs_root_mkdir(path, mode);
                        if (ret < 0) {
                                return ret;
                        }
                        ret = vfs_root_lookup(path, &ino);
                        if (ret < 0) {
                                return ret;
                        }
                } else {
                        u32 create_mode = (mode & 0777u) | VFS_S_IFREG;

                        ret = vfs_root_create_file(path, create_mode, &ino);
                        if (ret < 0) {
                                return ret;
                        }
                }
        } else {
                if ((flags & (VFS_O_CREAT | VFS_O_EXCL))
                    == (VFS_O_CREAT | VFS_O_EXCL)) {
                        return -LINUX_EEXIST;
                }

                if ((flags & VFS_O_DIRECTORY) && !ino.is_dir) {
                        return -LINUX_ENOTDIR;
                }

                if (!(flags & VFS_O_DIRECTORY) && ino.is_dir
                    && acc != VFS_O_RDONLY) {
                        return -LINUX_EISDIR;
                }

                if ((flags & VFS_O_TRUNC) && !ino.is_dir
                    && (acc == VFS_O_WRONLY || acc == VFS_O_RDWR)) {
                        if (!ino.writable) {
                                return -LINUX_EROFS;
                        }
                        ret = vfs_root_truncate(&ino, 0);
                        if (ret < 0) {
                                return ret;
                        }
                }
        }

        if (ino.is_dir && acc != VFS_O_RDONLY) {
                if (acc == VFS_O_WRONLY || acc == VFS_O_RDWR) {
                        return -LINUX_EISDIR;
                }
        }

        if (!ino.is_dir && acc != VFS_O_RDONLY) {
                if (!ino.writable) {
                        return -LINUX_EACCES;
                }
        }

        handle = vfs_handle_open(&ino, flags);
        if (handle == 0) {
                return -LINUX_EMFILE;
        }

        /* Bit 31 marks directory opens so compat can set is_dir without RPC. */
        return (i64)handle | (ino.is_dir ? VFS_OPEN_RET_IS_DIR_BIT : 0);
}

i64 vfs_read_handle(pid_t pid, u32 handle, u64 user_buf, u64 count)
{
        Tcb_Base *task = vfs_task_for_pid(pid);
        vfs_open_handle_t *file;
        u8 chunk[VFS_READ_CHUNK];
        u64 remaining = count;
        u64 total = 0;
        i64 n;

        if (!task) {
                return -LINUX_ESRCH;
        }

        file = vfs_handle_get(handle);
        if (!file) {
                return -LINUX_EBADF;
        }

        if (file->ino.is_dir) {
                return -LINUX_EISDIR;
        }

        if ((file->open_flags & VFS_O_ACCMODE) == VFS_O_WRONLY) {
                return -LINUX_EBADF;
        }

        while (remaining > 0) {
                u64 chunk_len = remaining;

                if (chunk_len > VFS_READ_CHUNK) {
                        chunk_len = VFS_READ_CHUNK;
                }

                n = vfs_root_read(&file->ino, file->offset, chunk, chunk_len);
                if (n < 0) {
                        return n;
                }
                if (n == 0) {
                        break;
                }

                if (linux_mm_store_to_user(
                            task->vs, user_buf + total, chunk, (size_t)n)
                    != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }

                file->offset += (u64)n;
                total += (u64)n;
                remaining -= (u64)n;

                if ((u64)n < chunk_len) {
                        break;
                }
        }

        return (i64)total;
}

i64 vfs_write_handle(pid_t pid, u32 handle, u64 user_buf, u64 count)
{
        Tcb_Base *task = vfs_task_for_pid(pid);
        vfs_open_handle_t *file;
        u8 chunk[VFS_READ_CHUNK];
        u64 remaining = count;
        u64 total = 0;
        i64 n;

        if (!task) {
                return -LINUX_ESRCH;
        }

        file = vfs_handle_get(handle);
        if (!file) {
                return -LINUX_EBADF;
        }

        if (file->ino.is_dir) {
                return -LINUX_EISDIR;
        }

        if ((file->open_flags & VFS_O_ACCMODE) == VFS_O_RDONLY) {
                return -LINUX_EBADF;
        }

        if (file->open_flags & VFS_O_APPEND) {
                file->offset = file->ino.size;
        }

        while (remaining > 0) {
                u64 chunk_len = remaining;

                if (chunk_len > VFS_READ_CHUNK) {
                        chunk_len = VFS_READ_CHUNK;
                }

                if (linux_mm_load_from_user(task->vs,
                                            user_buf + total,
                                            chunk,
                                            (size_t)chunk_len)
                    != REND_SUCCESS) {
                        return total > 0 ? (i64)total : -LINUX_EFAULT;
                }

                n = vfs_root_write(&file->ino, file->offset, chunk, chunk_len);
                if (n < 0) {
                        return total > 0 ? (i64)total : n;
                }
                if (n == 0) {
                        break;
                }

                file->offset += (u64)n;
                total += (u64)n;
                remaining -= (u64)n;

                if ((u64)n < chunk_len) {
                        break;
                }
        }

        return (i64)total;
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
        Tcb_Base *task = vfs_task_for_pid(pid);
        vfs_open_handle_t *file;
        vfs_kstat_t st;

        if (!task) {
                return -LINUX_ESRCH;
        }

        file = vfs_handle_get(handle);
        if (!file) {
                return -LINUX_EBADF;
        }

        vfs_kstat_from_inode(&file->ino, &st);
        return vfs_store_kstat(task, user_statbuf, &st);
}

i64 vfs_stat_path(pid_t pid, const char *path, u64 user_statbuf, i32 flags)
{
        Tcb_Base *task = vfs_task_for_pid(pid);
        vfs_inode_t ino;
        vfs_kstat_t st;
        i64 ret;

        (void)flags;

        if (!task) {
                return -LINUX_ESRCH;
        }
        if (!path) {
                return -LINUX_EINVAL;
        }

        ret = vfs_root_lookup(path, &ino);
        if (ret < 0) {
                return ret;
        }

        vfs_kstat_from_inode(&ino, &st);
        return vfs_store_kstat(task, user_statbuf, &st);
}

i64 vfs_mkdir_path(const char *path, u32 mode)
{
        if (!path) {
                return -LINUX_EINVAL;
        }

        return vfs_root_mkdir(path, mode);
}

i64 vfs_unlink_path(const char *path, i32 flags)
{
        if (!path) {
                return -LINUX_EINVAL;
        }

        if (flags & 0x200) {
                return -LINUX_ENOSYS;
        }

        return vfs_root_unlink(path);
}

i64 vfs_rename_path(const char *path, const char *newpath, i32 flags)
{
        (void)flags;

        if (!path || !newpath) {
                return -LINUX_EINVAL;
        }

        return vfs_root_rename(path, newpath);
}

i64 vfs_link_path(const char *path, const char *newpath, i32 flags)
{
        (void)flags;

        if (!path || !newpath) {
                return -LINUX_EINVAL;
        }

        return vfs_root_link(path, newpath);
}

i64 vfs_validate_dir(const char *path)
{
        vfs_inode_t ino;
        i64 ret;

        if (!path) {
                return -LINUX_EINVAL;
        }

        ret = vfs_root_lookup(path, &ino);
        if (ret < 0) {
                return ret;
        }
        if (!ino.is_dir) {
                return -LINUX_ENOTDIR;
        }

        return 0;
}

#define VFS_DIRENT64_HDR 19u

static u16 vfs_dirent64_reclen(u64 name_len)
{
        u16 reclen = (u16)(VFS_DIRENT64_HDR + name_len + 1);

        return (u16)((reclen + 7u) & ~7u);
}

i64 vfs_getdents64_handle(pid_t pid, u32 handle, u64 user_dirp, u64 count)
{
        Tcb_Base *task = vfs_task_for_pid(pid);
        vfs_open_handle_t *file;
        u8 chunk[512];
        u64 written = 0;
        u64 index;

        if (!task) {
                return -LINUX_ESRCH;
        }
        if (count == 0) {
                return 0;
        }

        file = vfs_handle_get(handle);
        if (!file) {
                return -LINUX_EBADF;
        }
        if (!file->ino.is_dir) {
                return -LINUX_ENOTDIR;
        }

        index = file->offset;

        while (written < count) {
                vfs_dirent_t ent;
                u64 name_len;
                u16 reclen;
                u64 next_index;
                i64 rd;
                error_t e;

                rd = vfs_root_readdir(file->ino.path, index, &ent);
                if (rd < 0) {
                        return rd;
                }
                if (rd > 0) {
                        break;
                }

                name_len = strlen(ent.name);
                reclen = vfs_dirent64_reclen(name_len);
                if (written + reclen > count) {
                        if (written == 0) {
                                return -LINUX_EINVAL;
                        }
                        break;
                }
                if (reclen > sizeof(chunk)) {
                        return -LINUX_EINVAL;
                }

                memset(chunk, 0, reclen);
                memcpy(chunk, &ent.d_ino, sizeof(ent.d_ino));
                next_index = index + 1;
                memcpy(chunk + 8, &next_index, sizeof(next_index));
                memcpy(chunk + 16, &reclen, sizeof(reclen));
                chunk[18] = ent.d_type;
                memcpy(chunk + VFS_DIRENT64_HDR, ent.name, name_len + 1);

                e = linux_mm_store_to_user(
                        task->vs, user_dirp + written, chunk, reclen);
                if (e != REND_SUCCESS) {
                        return written > 0 ? (i64)written : -LINUX_EFAULT;
                }

                written += reclen;
                index = next_index;
        }

        file->offset = index;
        return (i64)written;
}
