#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/time/linux_ktime.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

i64 sys_time(u64 user_tloc)
{
        Tcb_Base *task = get_cpu_current_task();
        VSpace *vs;
        u64 realtime_us;
        i64 secs;
        error_t e;

        if (!task || !task->vs) {
                return -LINUX_ESRCH;
        }

        vs = task->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        realtime_us = linux_time_realtime_us();
        secs = (i64)(realtime_us / 1000000ULL);

        if (user_tloc) {
                e = linux_mm_store_to_user(vs, user_tloc, &secs, sizeof(secs));
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }
        }

        return secs;
}
