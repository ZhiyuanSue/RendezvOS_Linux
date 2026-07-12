/*
 * File system syscalls — per-process fd table + VFS IPC (scheme B).
 * See doc/linux_compat/FD_TABLE.md
 */

#include <common/string.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/fs_ipc.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/fs/linux_pipe.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/linux_mm_radix.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

#define LINUX_O_CREAT     0x40
#define LINUX_O_DIRECTORY 0x10000
#if defined(_AARCH64_)
#define LINUX_O_DIRECTORY_AARCH64 0x4000
#endif

static i32 linux_open_flags_normalize(i32 flags)
{
        i32 out = flags;

        if ((out & LINUX_O_CREAT) == 0 && (out & 0x40) != 0) {
                out |= LINUX_O_CREAT;
        }
#if defined(_AARCH64_)
        if ((out & LINUX_O_DIRECTORY) == 0
            && (out & LINUX_O_DIRECTORY_AARCH64) != 0) {
                out |= LINUX_O_DIRECTORY;
        }
#endif
        return out;
}

static Tcb_Base *sys_fs_current(void)
{
        return get_cpu_current_task();
}

i64 sys_getcwd(u64 user_buf, u64 size)
{
        Tcb_Base *current = sys_fs_current();
        linux_fs_state_t *fs;
        VSpace *vs;
        error_t e;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        fs = linux_fs_state(current);
        if (!fs) {
                return -LINUX_ESRCH;
        }
        if (size == 0) {
                return -LINUX_EINVAL;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        {
                const char *cwd = linux_fs_cwd(fs);
                u64 len = strlen(cwd) + 1;

                if (size < len) {
                        return -LINUX_ERANGE;
                }

                e = linux_mm_store_to_user(vs, user_buf, cwd, (size_t)len);
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }

                return (i64)user_buf;
        }
}

i64 sys_dup(i32 fd)
{
        Tcb_Base *current = sys_fs_current();
        i32 newfd;

        newfd = linux_fd_lowest_free(current);
        if (newfd < 0) {
                return -LINUX_EMFILE;
        }

        return linux_fd_dup2(current, fd, newfd);
}

i64 sys_dup2(i32 oldfd, i32 newfd)
{
        return linux_fd_dup2(sys_fs_current(), oldfd, newfd);
}

i64 sys_openat(i32 dirfd, u64 user_pathname, i32 flags, u64 mode)
{
        Tcb_Base *current = sys_fs_current();
        VSpace *vs;
        char pathname[LINUX_VFS_PATH_MAX];
        char abs[LINUX_VFS_PATH_MAX];
        linux_fd_entry_t ent;
        i64 handle;
        i32 fd;
        error_t e;
        i64 ret;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(
                vs, user_pathname, pathname, sizeof(pathname));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        pathname[sizeof(pathname) - 1] = '\0';
        flags = linux_open_flags_normalize(flags);

        ret = linux_vfs_resolve_path(
                current, dirfd, pathname, abs, sizeof(abs));
        if (ret < 0) {
                return ret;
        }

        handle = vfs_ipc_request_response(
                KMSG_OP_VFS_OPEN, VFS_KMSG_FMT_OPEN, abs, flags, (u32)mode);
        if (handle <= 0) {
                return handle < 0 ? handle : -LINUX_EMFILE;
        }

        memset(&ent, 0, sizeof(ent));
        ent.kind = LINUX_FD_VFS;
        ent.vfs_handle = (u32)(handle & VFS_OPEN_RET_HANDLE_MASK);
        strncpy(ent.vfs_abs_path, abs, sizeof(ent.vfs_abs_path) - 1);
        ent.vfs_abs_path[sizeof(ent.vfs_abs_path) - 1] = '\0';
        if ((handle & VFS_OPEN_RET_IS_DIR_BIT) != 0) {
                ent.is_dir = true;
        } else if ((flags & LINUX_O_DIRECTORY) != 0) {
                ent.is_dir = true;
        } else if (vfs_ipc_request_response(KMSG_OP_VFS_VALIDATE_DIR,
                                            VFS_KMSG_FMT_VALIDATE_DIR,
                                            abs)
                   == 0) {
                ent.is_dir = true;
        }
        fd = linux_fd_alloc(current, &ent);
        if (fd < 0) {
                (void)vfs_ipc_request_response(
                        KMSG_OP_VFS_CLOSE,
                        VFS_KMSG_FMT_CLOSE,
                        ent.vfs_handle);
                return -LINUX_EMFILE;
        }

        return (i64)fd;
}

i64 sys_close(i32 fd)
{
        return linux_fd_close(sys_fs_current(), fd);
}

i64 sys_read(i32 fd, u64 user_buf, u64 count)
{
        Tcb_Base *current = sys_fs_current();
        linux_fd_entry_t *ent;

        ent = linux_fd_get(current, fd);
        if (!ent) {
                return -LINUX_EBADF;
        }

        switch (ent->kind) {
        case LINUX_FD_CONSOLE_IN:
                return 0;
        case LINUX_FD_CONSOLE_OUT:
        case LINUX_FD_CONSOLE_ERR:
                return -LINUX_EBADF;
        case LINUX_FD_PIPE:
                if (!ent->pipe_read) {
                        return -LINUX_EBADF;
                }
                return linux_pipe_read(current, ent->vfs_handle, user_buf, count);
        case LINUX_FD_VFS:
                return vfs_ipc_request_response(KMSG_OP_VFS_READ,
                                                VFS_KMSG_FMT_READ,
                                                ent->vfs_handle,
                                                user_buf,
                                                count);
        default:
                return -LINUX_EBADF;
        }
}

i64 sys_write(i32 fd, u64 user_buf, u64 count)
{
        Tcb_Base *current = sys_fs_current();
        linux_fd_entry_t *ent;

        ent = linux_fd_get(current, fd);
        if (!ent) {
                return -LINUX_EBADF;
        }

        switch (ent->kind) {
        case LINUX_FD_CONSOLE_OUT:
                return sys_write_impl(1, user_buf, count);
        case LINUX_FD_CONSOLE_ERR:
                return sys_write_impl(2, user_buf, count);
        case LINUX_FD_PIPE:
                if (ent->pipe_read) {
                        return -LINUX_EBADF;
                }
                return linux_pipe_write(current, ent->vfs_handle, user_buf, count);
        case LINUX_FD_VFS:
                return vfs_ipc_request_response(KMSG_OP_VFS_WRITE,
                                                VFS_KMSG_FMT_WRITE,
                                                ent->vfs_handle,
                                                user_buf,
                                                count);
        default:
                return -LINUX_EBADF;
        }
}

i64 sys_fstat(i32 fd, u64 user_statbuf)
{
        Tcb_Base *current = sys_fs_current();
        linux_fd_entry_t *ent;

        ent = linux_fd_get(current, fd);
        if (!ent) {
                return -LINUX_EBADF;
        }

        if (ent->kind != LINUX_FD_VFS) {
                return -LINUX_EBADF;
        }

        return vfs_ipc_request_response(KMSG_OP_VFS_FSTAT,
                                        VFS_KMSG_FMT_FSTAT,
                                        ent->vfs_handle,
                                        user_statbuf);
}

i64 sys_stat(u64 user_pathname, u64 user_statbuf)
{
        (void)user_pathname;
        (void)user_statbuf;
        return -LINUX_ENOSYS;
}

i64 sys_lseek(i32 fd, i64 offset, i32 whence)
{
        Tcb_Base *current = sys_fs_current();
        linux_fd_entry_t *ent;

        ent = linux_fd_get(current, fd);
        if (!ent) {
                return -LINUX_EBADF;
        }

        if (ent->kind != LINUX_FD_VFS) {
                return -LINUX_ESPIPE;
        }

        return vfs_ipc_request_response(KMSG_OP_VFS_LSEEK,
                                        VFS_KMSG_FMT_LSEEK,
                                        ent->vfs_handle,
                                        (u64)offset,
                                        (u32)whence);
}

i64 sys_chdir(u64 user_pathname)
{
        Tcb_Base *current = sys_fs_current();
        linux_fs_state_t *fs;
        VSpace *vs;
        char pathname[LINUX_VFS_PATH_MAX];
        char abs[LINUX_VFS_PATH_MAX];
        error_t e;
        i64 ret;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        fs = linux_fs_state(current);
        if (!fs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(
                vs, user_pathname, pathname, sizeof(pathname));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        pathname[sizeof(pathname) - 1] = '\0';

        ret = linux_vfs_resolve_path(
                current, LINUX_AT_FDCWD, pathname, abs, sizeof(abs));
        if (ret < 0) {
                return ret;
        }

        ret = vfs_ipc_request_response(
                KMSG_OP_VFS_CHDIR, VFS_KMSG_FMT_CHDIR, abs);
        if (ret < 0) {
                return ret;
        }

        linux_fs_set_cwd(fs, abs);
        return 0;
}

i64 sys_mkdir(u64 user_pathname, u32 mode)
{
        (void)user_pathname;
        (void)mode;
        return -LINUX_ENOSYS;
}

i64 sys_unlink(u64 user_pathname)
{
        return sys_unlinkat(LINUX_AT_FDCWD, user_pathname, 0);
}

i64 sys_getdents64(i32 fd, u64 user_dirp, u64 count)
{
        Tcb_Base *current = sys_fs_current();
        linux_fd_entry_t *ent;

        ent = linux_fd_get(current, fd);
        if (!ent) {
                return -LINUX_EBADF;
        }

        if (ent->kind != LINUX_FD_VFS || !ent->is_dir) {
                return -LINUX_ENOTDIR;
        }

        return vfs_ipc_request_response(KMSG_OP_VFS_GETDENTS64,
                                        VFS_KMSG_FMT_GETDENTS64,
                                        ent->vfs_handle,
                                        user_dirp,
                                        count);
}

i64 sys_pipe(u64 user_pipefd)
{
        return linux_pipe_create2(sys_fs_current(), user_pipefd, 0);
}

i64 sys_pipe2(u64 user_pipefd, i32 flags)
{
        return linux_pipe_create2(sys_fs_current(), user_pipefd, flags);
}

i64 sys_mkdirat(i32 dirfd, u64 user_pathname, u32 mode)
{
        Tcb_Base *current = sys_fs_current();
        VSpace *vs;
        char pathname[LINUX_VFS_PATH_MAX];
        char abs[LINUX_VFS_PATH_MAX];
        error_t e;
        i64 ret;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(
                vs, user_pathname, pathname, sizeof(pathname));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        pathname[sizeof(pathname) - 1] = '\0';

        ret = linux_vfs_resolve_path(
                current, dirfd, pathname, abs, sizeof(abs));
        if (ret < 0) {
                return ret;
        }

        return vfs_ipc_request_response(
                KMSG_OP_VFS_MKDIRAT, VFS_KMSG_FMT_MKDIRAT, abs, mode);
}

i64 sys_unlinkat(i32 dirfd, u64 user_pathname, i32 flags)
{
        Tcb_Base *current = sys_fs_current();
        VSpace *vs;
        char pathname[LINUX_VFS_PATH_MAX];
        char abs[LINUX_VFS_PATH_MAX];
        error_t e;
        i64 ret;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(
                vs, user_pathname, pathname, sizeof(pathname));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        pathname[sizeof(pathname) - 1] = '\0';

        ret = linux_vfs_resolve_path(
                current, dirfd, pathname, abs, sizeof(abs));
        if (ret < 0) {
                return ret;
        }

        return vfs_ipc_request_response(
                KMSG_OP_VFS_UNLINKAT, VFS_KMSG_FMT_UNLINKAT, abs, flags);
}

i64 sys_renameat(i32 olddirfd, u64 user_oldpath, i32 newdirfd,
                 u64 user_newpath, u32 flags)
{
        Tcb_Base *current = sys_fs_current();
        VSpace *vs;
        char oldpath[LINUX_VFS_PATH_MAX];
        char newpath[LINUX_VFS_PATH_MAX];
        char old_abs[LINUX_VFS_PATH_MAX];
        char new_abs[LINUX_VFS_PATH_MAX];
        error_t e;
        i64 ret;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(
                vs, user_oldpath, oldpath, sizeof(oldpath));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        oldpath[sizeof(oldpath) - 1] = '\0';

        e = linux_mm_load_from_user(
                vs, user_newpath, newpath, sizeof(newpath));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        newpath[sizeof(newpath) - 1] = '\0';

        ret = linux_vfs_resolve_path(
                current, olddirfd, oldpath, old_abs, sizeof(old_abs));
        if (ret < 0) {
                return ret;
        }

        ret = linux_vfs_resolve_path(
                current, newdirfd, newpath, new_abs, sizeof(new_abs));
        if (ret < 0) {
                return ret;
        }

        return vfs_ipc_request_response(KMSG_OP_VFS_RENAMEAT,
                                        VFS_KMSG_FMT_RENAMEAT,
                                        old_abs,
                                        new_abs,
                                        flags);
}

i64 sys_linkat(i32 olddirfd, u64 user_oldpath, i32 newdirfd, u64 user_newpath,
               i32 flags)
{
        Tcb_Base *current = sys_fs_current();
        VSpace *vs;
        char oldpath[LINUX_VFS_PATH_MAX];
        char newpath[LINUX_VFS_PATH_MAX];
        char old_abs[LINUX_VFS_PATH_MAX];
        char new_abs[LINUX_VFS_PATH_MAX];
        error_t e;
        i64 ret;

        (void)flags;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(
                vs, user_oldpath, oldpath, sizeof(oldpath));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        oldpath[sizeof(oldpath) - 1] = '\0';

        e = linux_mm_load_from_user(
                vs, user_newpath, newpath, sizeof(newpath));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        newpath[sizeof(newpath) - 1] = '\0';

        ret = linux_vfs_resolve_path(
                current, olddirfd, oldpath, old_abs, sizeof(old_abs));
        if (ret < 0) {
                return ret;
        }

        ret = linux_vfs_resolve_path(
                current, newdirfd, newpath, new_abs, sizeof(new_abs));
        if (ret < 0) {
                return ret;
        }

        return vfs_ipc_request_response(KMSG_OP_VFS_LINKAT,
                                        VFS_KMSG_FMT_LINKAT,
                                        old_abs,
                                        new_abs,
                                        (u32)flags);
}

i64 sys_newfstatat(i32 dirfd, u64 user_pathname, u64 user_statbuf, i32 flags)
{
        Tcb_Base *current = sys_fs_current();
        VSpace *vs;
        char pathname[LINUX_VFS_PATH_MAX];
        char abs[LINUX_VFS_PATH_MAX];
        error_t e;
        i64 ret;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(
                vs, user_pathname, pathname, sizeof(pathname));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        pathname[sizeof(pathname) - 1] = '\0';

        ret = linux_vfs_resolve_path(
                current, dirfd, pathname, abs, sizeof(abs));
        if (ret < 0) {
                return ret;
        }

        return vfs_ipc_request_response(KMSG_OP_VFS_NEWFSTATAT,
                                        VFS_KMSG_FMT_NEWFSTATAT,
                                        abs,
                                        user_statbuf,
                                        (u32)flags);
}

i64 sys_readlinkat(i32 dirfd, u64 user_pathname, u64 user_buf, u64 bufsiz)
{
        Tcb_Base *current = sys_fs_current();
        VSpace *vs;
        char pathname[LINUX_VFS_PATH_MAX];
        char abs[LINUX_VFS_PATH_MAX];
        error_t e;
        i64 ret;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(
                vs, user_pathname, pathname, sizeof(pathname));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        pathname[sizeof(pathname) - 1] = '\0';

        ret = linux_vfs_resolve_path(
                current, dirfd, pathname, abs, sizeof(abs));
        if (ret < 0) {
                return ret;
        }

        return vfs_ipc_request_response(KMSG_OP_VFS_READLINKAT,
                                        VFS_KMSG_FMT_READLINKAT,
                                        abs,
                                        user_buf,
                                        bufsiz);
}

i64 sys_faccessat(i32 dirfd, u64 user_pathname, i32 mode, i32 flags)
{
        Tcb_Base *current = sys_fs_current();
        VSpace *vs;
        char pathname[LINUX_VFS_PATH_MAX];
        char abs[LINUX_VFS_PATH_MAX];
        error_t e;
        i64 ret;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(
                vs, user_pathname, pathname, sizeof(pathname));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        pathname[sizeof(pathname) - 1] = '\0';

        ret = linux_vfs_resolve_path(
                current, dirfd, pathname, abs, sizeof(abs));
        if (ret < 0) {
                return ret;
        }

        return vfs_ipc_request_response(KMSG_OP_VFS_FACCESSAT,
                                        VFS_KMSG_FMT_FACCESSAT,
                                        abs,
                                        (u32)mode,
                                        (u32)flags);
}

i64 sys_dup3(i32 oldfd, i32 newfd, i32 flags)
{
        (void)flags;
        return linux_fd_dup2(sys_fs_current(), oldfd, newfd);
}
