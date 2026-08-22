#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/time/linux_time_types.h>
#include <rendezvos/error.h>
#include <rendezvos/task/thread.h>
#include <rendezvos/time.h>
#include <syscall.h>

i64 sys_times(u64 user_buf)
{
        linux_proc_resource_t *task = linux_current_proc();
        VSpace *vs;
        linux_tms_t tms;
        error_t e;

        if (!task || !linux_current_vs()) {
                return -LINUX_ESRCH;
        }
        vs = linux_current_vs();
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

        return jeffies_get();
}
