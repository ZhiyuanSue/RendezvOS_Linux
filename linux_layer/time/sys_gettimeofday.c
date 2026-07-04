#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/time/linux_ktime.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>
#include <syscall_entry.h>

i64 sys_gettimeofday(u64 user_tv, u64 user_tz)
{
        Tcb_Base *task = get_cpu_current_task();
        VSpace *vs;
        error_t e;
        u64 realtime_us;

        if (!task || !task->vs) {
                return -LINUX_ESRCH;
        }
        vs = task->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        realtime_us = linux_time_realtime_us();

        if (user_tv) {
                linux_timeval_t tv;

                linux_us_to_timeval(realtime_us, &tv);
                e = linux_mm_store_to_user(vs, user_tv, &tv, sizeof(tv));
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }
        }

        if (user_tz) {
                linux_timezone_t tz = {0, 0};

                e = linux_mm_store_to_user(vs, user_tz, &tz, sizeof(tz));
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }
        }

        return 0;
}
