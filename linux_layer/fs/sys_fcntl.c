/*
 * fcntl(2) — fd flags / status flags / DUPFD for BusyBox ash.
 */

#include <linux_compat/errno.h>
#include <linux_compat/fs/linux_fcntl.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

i64 sys_fcntl(i32 fd, i32 cmd, u64 arg)
{
        Tcb_Base *current = get_cpu_current_task();
        linux_fd_entry_t *ent;
        i32 newfd;
        i64 ret;

        ent = linux_fd_get(current, fd);
        if (!ent) {
                return -LINUX_EBADF;
        }

        switch (cmd) {
        case LINUX_F_GETFD:
                return (i64)(ent->fd_flags & LINUX_FD_CLOEXEC);

        case LINUX_F_SETFD:
                ent->fd_flags = ((u32)arg & LINUX_FD_CLOEXEC);
                if (linux_fd_store(current, fd, ent) != REND_SUCCESS) {
                        return -LINUX_EBADF;
                }
                return 0;

        case LINUX_F_GETFL:
                return (i64)ent->open_flags;

        case LINUX_F_SETFL:
                ent->open_flags = (ent->open_flags & ~(u32)LINUX_F_SETFL_MASK)
                                  | ((u32)arg & (u32)LINUX_F_SETFL_MASK);
                if (linux_fd_store(current, fd, ent) != REND_SUCCESS) {
                        return -LINUX_EBADF;
                }
                return 0;

        case LINUX_F_DUPFD:
        case LINUX_F_DUPFD_CLOEXEC:
                if ((i64)arg < 0 || arg > 0x7fffffffu) {
                        return -LINUX_EINVAL;
                }
                newfd = linux_fd_lowest_free_from(current, (i32)arg);
                if (newfd < 0) {
                        return -LINUX_EMFILE;
                }
                ret = linux_fd_dup2(current, fd, newfd);
                if (ret < 0) {
                        return ret;
                }
                if (cmd == LINUX_F_DUPFD_CLOEXEC) {
                        ent = linux_fd_get(current, newfd);
                        if (!ent) {
                                return -LINUX_EBADF;
                        }
                        ent->fd_flags |= LINUX_FD_CLOEXEC;
                        if (linux_fd_store(current, newfd, ent)
                            != REND_SUCCESS) {
                                return -LINUX_EBADF;
                        }
                }
                return ret;

        default:
                return -LINUX_EINVAL;
        }
}
