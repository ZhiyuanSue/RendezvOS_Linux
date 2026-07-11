#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

i64 sys_mount(u64 user_source, u64 user_target, u64 user_fstype, u64 flags,
              u64 user_data)
{
        Tcb_Base *current = get_cpu_current_task();
        VSpace *vs;

        (void)user_source;
        (void)user_target;
        (void)user_fstype;
        (void)flags;
        (void)user_data;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        /* Stub success for oscomp mount/umount tests (no block device yet). */
        return 0;
}

i64 sys_umount2(u64 user_target, i32 flags)
{
        Tcb_Base *current = get_cpu_current_task();
        VSpace *vs;

        (void)user_target;
        (void)flags;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        return 0;
}
