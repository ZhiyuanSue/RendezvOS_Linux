#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/time/linux_time_types.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/time.h>
#include <syscall.h>

extern volatile i64 jeffies;

i64 sys_times(u64 user_buf)
{
        Tcb_Base *task = get_cpu_current_task();
        VSpace *vs;
        linux_tms_t tms;
        error_t e;

        if (!task || !task->vs) {
                return -LINUX_ESRCH;
        }
        vs = task->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        tms.tms_utime = 0;
        tms.tms_stime = 0;
        tms.tms_cutime = 0;
        tms.tms_cstime = 0;

        if (user_buf) {
                e = linux_mm_store_to_user(vs, user_buf, &tms, sizeof(tms));
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }
        }

        return (i64)jeffies;
}
