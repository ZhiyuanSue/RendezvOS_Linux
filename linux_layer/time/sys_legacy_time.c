#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/time/linux_ktime.h>
#include <rendezvos/error.h>
#include <rendezvos/task/thread.h>
#include <syscall.h>

i64 sys_time(u64 user_tloc)
{
        linux_proc_resource_t *task = linux_current_proc();
        VSpace *vs;
        u64 realtime_us;
        i64 secs;
        error_t e;

        if (!task || !linux_current_vs()) {
                return -LINUX_ESRCH;
        }

        vs = linux_current_vs();
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
