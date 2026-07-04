#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/time/linux_ktime.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>
#include <syscall_entry.h>

i64 sys_clock_gettime(i32 clockid, u64 user_tp)
{
        Tcb_Base *task = get_cpu_current_task();
        VSpace *vs;
        linux_timespec_t ts;
        u64 us;
        error_t e;

        if (clockid != LINUX_CLOCK_REALTIME
            && clockid != LINUX_CLOCK_MONOTONIC) {
                return -LINUX_EINVAL;
        }
        if (!user_tp) {
                return -LINUX_EFAULT;
        }

        if (!task || !task->vs) {
                return -LINUX_ESRCH;
        }
        vs = task->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        us = (clockid == LINUX_CLOCK_MONOTONIC) ? linux_time_monotonic_us() :
                                                  linux_time_realtime_us();
        linux_us_to_timespec(us, &ts);

        e = linux_mm_store_to_user(vs, user_tp, &ts, sizeof(ts));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        return 0;
}
