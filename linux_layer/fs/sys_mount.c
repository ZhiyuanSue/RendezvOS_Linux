#include <common/string.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/fs/fs_ipc.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/linux_mm_radix.h>
#include <rendezvos/error.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

i64 sys_mount(u64 user_source, u64 user_target, u64 user_fstype, u64 flags,
              u64 user_data)
{
        Tcb_Base *current = get_cpu_current_task();
        VSpace *vs;
        char source[256];
        char target[LINUX_VFS_PATH_MAX];
        char fstype[32];
        error_t e;

        (void)user_data;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        if (user_source) {
                e = linux_mm_load_from_user(vs, user_source, source,
                                            sizeof(source));
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }
                source[sizeof(source) - 1] = '\0';
        } else {
                source[0] = '\0';
        }

        e = linux_mm_load_from_user(vs, user_target, target, sizeof(target));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        target[sizeof(target) - 1] = '\0';

        e = linux_mm_load_from_user(vs, user_fstype, fstype, sizeof(fstype));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        fstype[sizeof(fstype) - 1] = '\0';

        (void)source;

        return vfs_ipc_request_response(KMSG_OP_VFS_MOUNT, VFS_KMSG_FMT_MOUNT,
                                        target, fstype, flags);
}

i64 sys_umount2(u64 user_target, i32 flags)
{
        Tcb_Base *current = get_cpu_current_task();
        VSpace *vs;
        char target[LINUX_VFS_PATH_MAX];
        error_t e;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(vs, user_target, target, sizeof(target));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        target[sizeof(target) - 1] = '\0';

        return vfs_ipc_request_response(KMSG_OP_VFS_UMOUNT, VFS_KMSG_FMT_UMOUNT,
                                        target, (u32)flags);
}
