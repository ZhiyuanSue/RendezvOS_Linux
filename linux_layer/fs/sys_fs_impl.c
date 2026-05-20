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
#include <modules/log/log.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

/*
 * Simple placeholder implementations for now
 * These will send IPC messages to vfs_server when it's ready
 */

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
                const char* fake_cwd = "/";
                size_t len = strlen(fake_cwd) + 1;

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
        pr_debug("[VFS] sys_dup called: fd=%d\n", fd);

        /* TODO: Send IPC request to vfs_server */
        return -LINUX_ENOSYS;
}

i64 sys_dup2(i32 oldfd, i32 newfd)
{
        pr_debug("[VFS] sys_dup2 called: oldfd=%d, newfd=%d\n", oldfd, newfd);

        /* TODO: Send IPC request to vfs_server */
        return -LINUX_ENOSYS;
}

i64 sys_openat(i32 dirfd, u64 user_pathname, i32 flags, u64 mode)
{
        Tcb_Base *current = get_cpu_current_task();
        VSpace *vs;
        char pathname[256];
        error_t e;

        pr_debug("[VFS] sys_openat called: dirfd=%d, pathname=0x%lx, flags=0x%x, mode=0x%lx\n",
                 dirfd, user_pathname, flags, mode);

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        /* Copy pathname from user space */
        e = linux_mm_load_from_user(vs, user_pathname, pathname, sizeof(pathname));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        pathname[sizeof(pathname) - 1] = '\0';

        pr_debug("[VFS] Opening file: %s\n", pathname);

        /* TODO: Send IPC request to vfs_server */
        return -LINUX_ENOSYS;
}

i64 sys_close(i32 fd)
{
        pr_debug("[VFS] sys_close called: fd=%d\n", fd);

        /* TODO: Send IPC request to vfs_server */
        return -LINUX_ENOSYS;
}

i64 sys_read(i32 fd, u64 user_buf, u64 count)
{
        pr_debug("[VFS] sys_read called: fd=%d, buf=0x%lx, count=%lu\n",
                 fd, user_buf, count);

        /* TODO: Send IPC request to vfs_server */
        return -LINUX_ENOSYS;
}

i64 sys_write(i32 fd, u64 user_buf, u64 count)
{
        pr_debug("[VFS] sys_write called: fd=%d, buf=0x%lx, count=%lu\n",
                 fd, user_buf, count);

        /* TODO: For now, use existing sys_write implementation */
        /* This will be migrated to IPC later */
        extern i64 sys_write_impl(i32 fd, u64 user_buf, u64 count);
        return sys_write_impl(fd, user_buf, count);
}

i64 sys_fstat(i32 fd, u64 user_statbuf)
{
        pr_debug("[VFS] sys_fstat called: fd=%d, statbuf=0x%lx\n", fd, user_statbuf);

        /* TODO: Send IPC request to vfs_server */
        return -LINUX_ENOSYS;
}

i64 sys_stat(u64 user_pathname, u64 user_statbuf)
{
        pr_debug("[VFS] sys_stat called: pathname=0x%lx, statbuf=0x%lx\n",
                 user_pathname, user_statbuf);

        /* TODO: Send IPC request to vfs_server */
        return -LINUX_ENOSYS;
}

i64 sys_lseek(i32 fd, i64 offset, i32 whence)
{
        pr_debug("[VFS] sys_lseek called: fd=%d, offset=%ld, whence=%d\n",
                 fd, offset, whence);

        /* TODO: Send IPC request to vfs_server */
        return -LINUX_ENOSYS;
}

i64 sys_chdir(u64 user_pathname)
{
        Tcb_Base *current = get_cpu_current_task();
        VSpace *vs;
        char pathname[256];
        error_t e;

        pr_debug("[VFS] sys_chdir called: pathname=0x%lx\n", user_pathname);

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        /* Copy pathname from user space */
        e = linux_mm_load_from_user(vs, user_pathname, pathname, sizeof(pathname));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        pathname[sizeof(pathname) - 1] = '\0';

        pr_debug("[VFS] Changing directory to: %s\n", pathname);

        /* TODO: Send IPC request to vfs_server */
        return -LINUX_ENOSYS;
}

i64 sys_mkdir(u64 user_pathname, u32 mode)
{
        Tcb_Base *current = get_cpu_current_task();
        VSpace *vs;
        char pathname[256];
        error_t e;

        pr_debug("[VFS] sys_mkdir called: pathname=0x%lx, mode=0%o\n",
                 user_pathname, mode);

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        /* Copy pathname from user space */
        e = linux_mm_load_from_user(vs, user_pathname, pathname, sizeof(pathname));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        pathname[sizeof(pathname) - 1] = '\0';

        pr_debug("[VFS] Creating directory: %s\n", pathname);

        /* TODO: Send IPC request to vfs_server */
        return -LINUX_ENOSYS;
}

i64 sys_unlink(u64 user_pathname)
{
        Tcb_Base *current = get_cpu_current_task();
        VSpace *vs;
        char pathname[256];
        error_t e;

        pr_debug("[VFS] sys_unlink called: pathname=0x%lx\n", user_pathname);

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        /* Copy pathname from user space */
        e = linux_mm_load_from_user(vs, user_pathname, pathname, sizeof(pathname));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        pathname[sizeof(pathname) - 1] = '\0';

        pr_debug("[VFS] Unlinking: %s\n", pathname);

        /* TODO: Send IPC request to vfs_server */
        return -LINUX_ENOSYS;
}

i64 sys_getdents64(i32 fd, u64 user_dirp, u64 count)
{
        pr_debug("[VFS] sys_getdents64 called: fd=%d, dirp=0x%lx, count=%lu\n",
                 fd, user_dirp, count);

        /* TODO: Send IPC request to vfs_server */
        return -LINUX_ENOSYS;
}

i64 sys_pipe(u64 user_pipefd)
{
        pr_debug("[VFS] sys_pipe called: pipefd=0x%lx\n", user_pipefd);

        /* TODO: Send IPC request to vfs_server */
        return -LINUX_ENOSYS;
}
