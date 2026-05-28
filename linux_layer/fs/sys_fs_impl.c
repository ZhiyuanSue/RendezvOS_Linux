/*
 * File System Syscall Implementations using IPC
 *
 * These syscalls use vfs_ipc_request_response to communicate with vfs_server.
 * Currently all return -ENOSYS until vfs_server is fully implemented.
 */

#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/fs_ipc.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/linux_mm_radix.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

i64 sys_getcwd(u64 user_buf, u64 size)
{
        Tcb_Base* current = get_cpu_current_task();
        VSpace* vs;
        error_t e;
        i64 ipc_ret;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        ipc_ret = vfs_ipc_request_response(KMSG_OP_VFS_GETCWD, VFS_KMSG_FMT_GETCWD,
                                           user_buf, size);
        if (ipc_ret < 0) {
                return ipc_ret;
        }
        if (ipc_ret != 0) {
                return ipc_ret;
        }

        {
                static const char fake_cwd[] = "/";
                size_t len = sizeof(fake_cwd);

                if (size < len) {
                        return -LINUX_ERANGE;
                }

                e = linux_mm_store_to_user(vs, user_buf, fake_cwd, len);
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }

                return (i64)user_buf;
        }
}

i64 sys_dup(i32 fd)
{
        (void)fd;
        return -LINUX_ENOSYS;
}

i64 sys_dup2(i32 oldfd, i32 newfd)
{
        (void)oldfd;
        (void)newfd;
        return -LINUX_ENOSYS;
}

i64 sys_openat(i32 dirfd, u64 user_pathname, i32 flags, u64 mode)
{
        Tcb_Base *current = get_cpu_current_task();
        VSpace *vs;
        char pathname[256];
        error_t e;

        (void)dirfd;
        (void)flags;
        (void)mode;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(vs, user_pathname, pathname, sizeof(pathname));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        pathname[sizeof(pathname) - 1] = '\0';

        (void)pathname;
        return -LINUX_ENOSYS;
}

i64 sys_close(i32 fd)
{
        (void)fd;
        return -LINUX_ENOSYS;
}

i64 sys_read(i32 fd, u64 user_buf, u64 count)
{
        (void)fd;
        (void)user_buf;
        (void)count;
        return -LINUX_ENOSYS;
}

i64 sys_write(i32 fd, u64 user_buf, u64 count)
{
        extern i64 sys_write_impl(i32 fd, u64 user_buf, u64 count);
        return sys_write_impl(fd, user_buf, count);
}

i64 sys_fstat(i32 fd, u64 user_statbuf)
{
        (void)fd;
        (void)user_statbuf;
        return -LINUX_ENOSYS;
}

i64 sys_stat(u64 user_pathname, u64 user_statbuf)
{
        (void)user_pathname;
        (void)user_statbuf;
        return -LINUX_ENOSYS;
}

i64 sys_lseek(i32 fd, i64 offset, i32 whence)
{
        (void)fd;
        (void)offset;
        (void)whence;
        return -LINUX_ENOSYS;
}

i64 sys_chdir(u64 user_pathname)
{
        Tcb_Base *current = get_cpu_current_task();
        VSpace *vs;
        char pathname[256];
        error_t e;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(vs, user_pathname, pathname, sizeof(pathname));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        pathname[sizeof(pathname) - 1] = '\0';
        (void)pathname;
        return -LINUX_ENOSYS;
}

i64 sys_mkdir(u64 user_pathname, u32 mode)
{
        Tcb_Base *current = get_cpu_current_task();
        VSpace *vs;
        char pathname[256];
        error_t e;

        (void)mode;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(vs, user_pathname, pathname, sizeof(pathname));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        pathname[sizeof(pathname) - 1] = '\0';
        (void)pathname;
        return -LINUX_ENOSYS;
}

i64 sys_unlink(u64 user_pathname)
{
        Tcb_Base *current = get_cpu_current_task();
        VSpace *vs;
        char pathname[256];
        error_t e;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(vs, user_pathname, pathname, sizeof(pathname));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        pathname[sizeof(pathname) - 1] = '\0';
        (void)pathname;
        return -LINUX_ENOSYS;
}

i64 sys_getdents64(i32 fd, u64 user_dirp, u64 count)
{
        (void)fd;
        (void)user_dirp;
        (void)count;
        return -LINUX_ENOSYS;
}

i64 sys_pipe(u64 user_pipefd)
{
        (void)user_pipefd;
        return -LINUX_ENOSYS;
}

i64 sys_pipe2(u64 user_pipefd, i32 flags)
{
        i64 ipc_ret;

        (void)user_pipefd;
        (void)flags;

        ipc_ret = vfs_ipc_request_response(KMSG_OP_VFS_PIPE2, VFS_KMSG_FMT_PIPE2,
                                          user_pipefd, flags);
        if (ipc_ret < 0) {
                return ipc_ret;
        }
        return ipc_ret;
}

i64 sys_mkdirat(i32 dirfd, u64 user_pathname, u32 mode)
{
        Tcb_Base* current = get_cpu_current_task();
        VSpace* vs;
        char pathname[256];
        error_t e;
        i64 ipc_ret;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(vs, user_pathname, pathname, sizeof(pathname));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        pathname[sizeof(pathname) - 1] = '\0';

        ipc_ret = vfs_ipc_request_response(KMSG_OP_VFS_MKDIRAT, VFS_KMSG_FMT_MKDIRAT,
                                          dirfd, pathname, mode);
        if (ipc_ret < 0) {
                return ipc_ret;
        }
        return ipc_ret;
}

i64 sys_unlinkat(i32 dirfd, u64 user_pathname, i32 flags)
{
        (void)dirfd;
        (void)user_pathname;
        (void)flags;
        return -LINUX_ENOSYS;
}

i64 sys_newfstatat(i32 dirfd, u64 user_pathname, u64 user_statbuf, i32 flags)
{
        (void)dirfd;
        (void)user_pathname;
        (void)user_statbuf;
        (void)flags;
        return -LINUX_ENOSYS;
}

i64 sys_dup3(i32 oldfd, i32 newfd, i32 flags)
{
        i64 ipc_ret;

        (void)oldfd;
        (void)newfd;
        (void)flags;

        ipc_ret = vfs_ipc_request_response(KMSG_OP_VFS_DUP3, VFS_KMSG_FMT_DUP3,
                                          oldfd, newfd, flags);
        if (ipc_ret < 0) {
                return ipc_ret;
        }
        return ipc_ret;
}
