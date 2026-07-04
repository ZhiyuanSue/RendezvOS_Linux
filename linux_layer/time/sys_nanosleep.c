#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/time/linux_ktime.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>
#include <syscall_entry.h>

#define LINUX_TIMER_ABSTIME 1

static i64 linux_timespec_validate(const linux_timespec_t *ts)
{
        if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000ll) {
                return -LINUX_EINVAL;
        }
        return 0;
}

static i64 linux_nanosleep_impl(i32 clockid, u64 user_req, u64 user_rem,
                                bool absolute)
{
        Tcb_Base *task = get_cpu_current_task();
        VSpace *vs;
        linux_timespec_t req;
        linux_timespec_t rem;
        tick_t deadline;
        bool already_satisfied = false;
        error_t e;
        i64 ret;

        if (!task || !task->vs) {
                return -LINUX_ESRCH;
        }
        vs = task->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }
        if (!user_req) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(vs, user_req, &req, sizeof(req));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        ret = linux_timespec_validate(&req);
        if (ret < 0) {
                return ret;
        }

        deadline = linux_time_resolve_deadline(
                clockid, &req, absolute, &already_satisfied);
        if (already_satisfied) {
                return 0;
        }

        ret = linux_time_sleep_until_count(deadline, &rem);
        if (ret == -LINUX_EINTR && user_rem) {
                e = linux_mm_store_to_user(vs, user_rem, &rem, sizeof(rem));
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }
        }
        return ret;
}

i64 sys_nanosleep(u64 user_req, u64 user_rem)
{
        return linux_nanosleep_impl(
                LINUX_CLOCK_MONOTONIC, user_req, user_rem, false);
}

i64 sys_clock_nanosleep(i32 clockid, i32 flags, u64 user_req, u64 user_rem)
{
        bool absolute = false;

        if (clockid != LINUX_CLOCK_REALTIME
            && clockid != LINUX_CLOCK_MONOTONIC) {
                return -LINUX_EINVAL;
        }
        if (flags & ~LINUX_TIMER_ABSTIME) {
                return -LINUX_EINVAL;
        }
        if (flags & LINUX_TIMER_ABSTIME) {
                absolute = true;
        }

        return linux_nanosleep_impl(clockid, user_req, user_rem, absolute);
}
