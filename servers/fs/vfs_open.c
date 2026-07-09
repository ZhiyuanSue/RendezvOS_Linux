/*
 * openat / read / lseek / stat front-end (vfs_server).
 */

#include "vfs_open.h"

#include "vfs_fd.h"
#include "vfs_path.h"
#include "vfs_root.h"

#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_registry.h>
#include <rendezvos/mm/vmm.h>

#define VFS_READ_CHUNK 4096u

#define VFS_S_IFREG 0100000u

static Tcb_Base *vfs_task_for_pid(pid_t pid)
{
        Tcb_Base *task = find_task_by_pid(pid);

        if (!task || !task->vs || !linux_vspace_is_user_table(task->vs)) {
                return NULL;
        }

        return task;
}

static i64 vfs_resolve_path(i32 dirfd, const char *path, char *out, u64 out_cap)
{
        char norm[VFS_PATH_MAX];

        if (!path || !out || out_cap == 0) {
                return -LINUX_EINVAL;
        }

        if (dirfd != VFS_AT_FDCWD) {
                return -LINUX_EBADF;
        }

        if (path[0] == '/') {
                vfs_path_normalize(path, out, out_cap);
                return 0;
        }

        {
                const char *rel = path;

                if (rel[0] == '.' && rel[1] == '/') {
                        rel += 2;
                }
                u64 i = 0;

                out[i++] = '/';
                for (u64 j = 0; rel[j] != '\0' && i + 1 < out_cap; j++) {
                        out[i++] = rel[j];
                }
                out[i] = '\0';
        }

        vfs_path_normalize(out, norm, sizeof(norm));
        strncpy(out, norm, out_cap - 1);
        out[out_cap - 1] = '\0';
        return 0;
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

i64 vfs_openat(pid_t pid, i32 dirfd, const char *path, i32 flags, u32 mode)
{
        char abs[VFS_PATH_MAX];
        vfs_inode_t ino;
        i64 ret;
        i32 acc = flags & VFS_O_ACCMODE;

        ret = vfs_resolve_path(dirfd, path, abs, sizeof(abs));
        if (ret < 0) {
                return ret;
        }

        ret = vfs_root_lookup(abs, &ino);

        if (ret < 0) {
                if (!(flags & VFS_O_CREAT)) {
                        return ret;
                }

                if (flags & VFS_O_DIRECTORY) {
                        ret = vfs_root_mkdir(abs, mode);
                        if (ret < 0) {
                                return ret;
                        }
                        ret = vfs_root_lookup(abs, &ino);
                        if (ret < 0) {
                                return ret;
                        }
                } else {
                        u32 create_mode = (mode & 0777u) | VFS_S_IFREG;

                        ret = vfs_root_create_file(abs, create_mode, &ino);
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

                if (!(flags & VFS_O_DIRECTORY) && ino.is_dir) {
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
                /* Directory fds are read-only for Phase 4 bootstrap. */
                if (acc == VFS_O_WRONLY || acc == VFS_O_RDWR) {
                        return -LINUX_EISDIR;
                }
        }

        if (!ino.is_dir && acc != VFS_O_RDONLY) {
                if (!ino.writable) {
                        return -LINUX_EACCES;
                }
        }

        return vfs_fd_open(pid, &ino, flags);
}

i64 vfs_read_fd(pid_t pid, i32 fd, u64 user_buf, u64 count)
{
        Tcb_Base *task = vfs_task_for_pid(pid);
        vfs_open_file_t *file;
        u8 chunk[VFS_READ_CHUNK];
        u64 remaining = count;
        u64 total = 0;
        i64 n;

        if (!task) {
                return -LINUX_ESRCH;
        }

        file = vfs_fd_get(pid, fd);
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

i64 vfs_write_fd(pid_t pid, i32 fd, u64 user_buf, u64 count)
{
        Tcb_Base *task = vfs_task_for_pid(pid);
        vfs_open_file_t *file;
        u8 chunk[VFS_READ_CHUNK];
        u64 remaining = count;
        u64 total = 0;
        i64 n;

        if (!task) {
                return -LINUX_ESRCH;
        }

        file = vfs_fd_get(pid, fd);
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

i64 vfs_lseek_fd(pid_t pid, i32 fd, i64 offset, i32 whence)
{
        vfs_open_file_t *file;
        i64 new_off;

        file = vfs_fd_get(pid, fd);
        if (!file) {
                return -LINUX_EBADF;
        }

        if (file->ino.is_dir) {
                return -LINUX_EISDIR;
        }

        switch (whence) {
        case 0: /* SEEK_SET */
                new_off = offset;
                break;
        case 1: /* SEEK_CUR */
                new_off = (i64)file->offset + offset;
                break;
        case 2: /* SEEK_END */
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

i64 vfs_fstat_fd(pid_t pid, i32 fd, u64 user_statbuf)
{
        Tcb_Base *task = vfs_task_for_pid(pid);
        vfs_open_file_t *file;
        vfs_kstat_t st;

        if (!task) {
                return -LINUX_ESRCH;
        }

        file = vfs_fd_get(pid, fd);
        if (!file) {
                return -LINUX_EBADF;
        }

        vfs_kstat_from_inode(&file->ino, &st);
        return vfs_store_kstat(task, user_statbuf, &st);
}

i64 vfs_statat(pid_t pid, i32 dirfd, const char *path, u64 user_statbuf,
               i32 flags)
{
        char abs[VFS_PATH_MAX];
        Tcb_Base *task = vfs_task_for_pid(pid);
        vfs_inode_t ino;
        vfs_kstat_t st;
        i64 ret;

        (void)flags;

        if (!task) {
                return -LINUX_ESRCH;
        }

        ret = vfs_resolve_path(dirfd, path, abs, sizeof(abs));
        if (ret < 0) {
                return ret;
        }

        ret = vfs_root_lookup(abs, &ino);
        if (ret < 0) {
                return ret;
        }

        vfs_kstat_from_inode(&ino, &st);
        return vfs_store_kstat(task, user_statbuf, &st);
}

i64 vfs_mkdirat(pid_t pid, i32 dirfd, const char *path, u32 mode)
{
        char abs[VFS_PATH_MAX];
        i64 ret;

        (void)pid;

        ret = vfs_resolve_path(dirfd, path, abs, sizeof(abs));
        if (ret < 0) {
                return ret;
        }

        return vfs_root_mkdir(abs, mode);
}

i64 vfs_unlinkat(pid_t pid, i32 dirfd, const char *path, i32 flags)
{
        char abs[VFS_PATH_MAX];
        i64 ret;

        (void)pid;

        if (flags & 0x200) { /* AT_REMOVEDIR — not yet */
                return -LINUX_ENOSYS;
        }

        ret = vfs_resolve_path(dirfd, path, abs, sizeof(abs));
        if (ret < 0) {
                return ret;
        }

        return vfs_root_unlink(abs);
}
