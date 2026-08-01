/*
 * poll / ppoll — fd readiness; sleep via per-thread sleep_port (IPC).
 *
 * Wait model: local readiness scan; if need to wait, sleep_port +
 * linux_time_sleep_until_count (same family as nanosleep).
 */

#include <common/string.h>
#include <linux_compat/debug_trace.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/fs/linux_poll.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/time/linux_ktime.h>
#include <linux_compat/time/linux_time_types.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/time.h>
#include <syscall.h>

/* Infinite poll: sleep in slices so signals / readiness can be rechecked. */
#define LINUX_POLL_INF_SLICE_MS 50

#if LINUX_COMPAT_TRACE_POLL
static const char *linux_poll_kind_name(linux_fd_kind_t kind)
{
        switch (kind) {
        case LINUX_FD_CONSOLE_IN:
                return "console_in";
        case LINUX_FD_CONSOLE_OUT:
                return "console_out";
        case LINUX_FD_CONSOLE_ERR:
                return "console_err";
        case LINUX_FD_VFS:
                return "vfs";
        case LINUX_FD_PIPE:
                return "pipe";
        default:
                return "none";
        }
}
#endif

static i16 linux_poll_revents_for(const linux_fd_entry_t *ent, i16 want)
{
        i16 got = 0;

        switch (ent->kind) {
        case LINUX_FD_CONSOLE_IN:
                /*
                 * No UART RX yet: sys_read returns 0. Treat as EOF device
                 * (POLLHUP), not POLLIN (spin) and not "never ready" (sleep).
                 */
                if ((want & (LINUX_POLLIN | LINUX_POLLPRI)) != 0) {
                        got |= LINUX_POLLHUP;
                }
                break;
        case LINUX_FD_CONSOLE_OUT:
        case LINUX_FD_CONSOLE_ERR:
                if ((want & LINUX_POLLOUT) != 0) {
                        got |= LINUX_POLLOUT;
                }
                break;
        case LINUX_FD_VFS:
        case LINUX_FD_PIPE:
                /* Optimistic until real buffer-empty waits exist. */
                if ((want & (LINUX_POLLIN | LINUX_POLLPRI)) != 0) {
                        got |= LINUX_POLLIN;
                }
                if ((want & LINUX_POLLOUT) != 0) {
                        got |= LINUX_POLLOUT;
                }
                break;
        default:
                return LINUX_POLLNVAL;
        }
        return got;
}

static i64 linux_poll_scan(Tcb_Base *task, VSpace *vs, u64 ufds, u32 nfds)
{
        struct allocator *alloc = percpu(kallocator);
        linux_pollfd_t *kfds;
        u32 i;
        i64 ready = 0;
        error_t e;

        if (nfds == 0) {
                return 0;
        }
        if (nfds > LINUX_POLL_MAX_NFDS) {
                return -LINUX_EINVAL;
        }
        if (!alloc) {
                return -LINUX_ENOMEM;
        }

        kfds = alloc->m_alloc(alloc, (size_t)nfds * sizeof(*kfds));
        if (!kfds) {
                return -LINUX_ENOMEM;
        }

        e = linux_mm_load_from_user(vs, ufds, kfds,
                                    (size_t)nfds * sizeof(*kfds));
        if (e != REND_SUCCESS) {
                alloc->m_free(alloc, kfds);
                return -LINUX_EFAULT;
        }

        for (i = 0; i < nfds; i++) {
                linux_fd_entry_t *ent;
                i16 got;

                kfds[i].revents = 0;
                if (kfds[i].fd < 0) {
                        continue;
                }

                ent = linux_fd_get(task, kfds[i].fd);
                if (!ent) {
                        kfds[i].revents = LINUX_POLLNVAL;
                        ready++;
#if LINUX_COMPAT_TRACE_POLL
                        pr_info("[poll] fd=%d kind=bad events=0x%x -> NVAL\n",
                                kfds[i].fd, (u32)kfds[i].events);
#endif
                        continue;
                }

                got = linux_poll_revents_for(ent, kfds[i].events);
                if (got == LINUX_POLLNVAL) {
                        kfds[i].revents = LINUX_POLLNVAL;
                        ready++;
                } else if (got != 0) {
                        kfds[i].revents = got;
                        ready++;
                }

#if LINUX_COMPAT_TRACE_POLL
                pr_info("[poll] fd=%d kind=%s events=0x%x revents=0x%x\n",
                        kfds[i].fd,
                        linux_poll_kind_name(ent->kind),
                        (u32)kfds[i].events,
                        (u32)kfds[i].revents);
#endif
        }

        e = linux_mm_store_to_user(vs, ufds, kfds,
                                   (size_t)nfds * sizeof(*kfds));
        alloc->m_free(alloc, kfds);
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        return ready;
}

static i64 linux_poll_sleep_ms(i32 timeout_ms)
{
        tick_t now;
        u64 us;

        if (timeout_ms <= 0) {
                return 0;
        }

        now = rendezvos_time_now();
        us = (u64)timeout_ms * 1000ull;
        return linux_time_sleep_until_count(
                now + rendezvos_time_us_to_count(us), NULL);
}

/* >=0 timeout_ms, or -LINUX_EINVAL. */
static i64 linux_timespec_to_timeout_ms(const linux_timespec_t *ts)
{
        i64 ms;

        if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000ll) {
                return -LINUX_EINVAL;
        }
        if (ts->tv_sec == 0 && ts->tv_nsec == 0) {
                return 0;
        }
        if (ts->tv_sec > (i64)(0x7fffffffll / 1000ll)) {
                return 0x7fffffff;
        }
        ms = ts->tv_sec * 1000ll + (ts->tv_nsec + 999999ll) / 1000000ll;
        if (ms <= 0) {
                return 1;
        }
        if (ms > 0x7fffffffll) {
                return 0x7fffffff;
        }
        return ms;
}

static i32 linux_poll_remaining_ms(tick_t deadline)
{
        tick_t now = rendezvos_time_now();
        u64 rem_us;
        i32 rem_ms;

        if (!time_before(now, deadline)) {
                return 0;
        }
        rem_us = rendezvos_time_count_to_us(deadline - now);
        if (rem_us == 0) {
                rem_us = 1;
        }
        rem_ms = (i32)((rem_us + 999ull) / 1000ull);
        return rem_ms > 0 ? rem_ms : 1;
}

i64 sys_poll(u64 ufds, u32 nfds, i32 timeout_ms)
{
        Tcb_Base *task = get_cpu_current_task();
        VSpace *vs;
        i64 ready;
        i64 wait_ret;
        tick_t deadline = 0;
        bool have_deadline = false;

        if (!task || !task->vs) {
                return -LINUX_ESRCH;
        }
        vs = task->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }
        if (ufds == 0 && nfds != 0) {
                return -LINUX_EFAULT;
        }

#if LINUX_COMPAT_TRACE_POLL
        pr_info("[poll] enter nfds=%u timeout_ms=%d\n", nfds, timeout_ms);
#endif

        if (timeout_ms > 0) {
                deadline = rendezvos_time_now()
                           + rendezvos_time_us_to_count(
                                   (u64)timeout_ms * 1000ull);
                have_deadline = true;
        }

        for (;;) {
                ready = linux_poll_scan(task, vs, ufds, nfds);
                if (ready < 0) {
#if LINUX_COMPAT_TRACE_POLL
                        pr_info("[poll] scan err=%ld\n", (long)ready);
#endif
                        return ready;
                }
                if (ready > 0) {
#if LINUX_COMPAT_TRACE_POLL
                        pr_info("[poll] return ready=%ld\n", (long)ready);
#endif
                        return ready;
                }

                if (timeout_ms == 0) {
#if LINUX_COMPAT_TRACE_POLL
                        pr_info("[poll] return 0 (timeout=0)\n");
#endif
                        return 0;
                }

                if (have_deadline) {
                        i32 rem_ms = linux_poll_remaining_ms(deadline);

                        if (rem_ms == 0) {
#if LINUX_COMPAT_TRACE_POLL
                                pr_info("[poll] return 0 (deadline)\n");
#endif
                                return 0;
                        }
#if LINUX_COMPAT_TRACE_POLL
                        pr_info("[poll] sleep rem_ms=%d\n", rem_ms);
#endif
                        wait_ret = linux_poll_sleep_ms(rem_ms);
                } else {
#if LINUX_COMPAT_TRACE_POLL
                        pr_info("[poll] sleep inf slice %d ms\n",
                                LINUX_POLL_INF_SLICE_MS);
#endif
                        wait_ret = linux_poll_sleep_ms(LINUX_POLL_INF_SLICE_MS);
                }

                if (wait_ret < 0) {
#if LINUX_COMPAT_TRACE_POLL
                        pr_info("[poll] sleep ret=%ld\n", (long)wait_ret);
#endif
                        return wait_ret;
                }
        }
}

i64 sys_ppoll(u64 ufds, u32 nfds, u64 user_tsp, u64 user_sigmask,
              u64 sigsetsize)
{
        Tcb_Base *task;
        VSpace *vs;
        linux_timespec_t ts;
        i64 ms;
        error_t e;

        (void)user_sigmask;
        (void)sigsetsize;

        if (!user_tsp) {
                return sys_poll(ufds, nfds, -1);
        }

        task = get_cpu_current_task();
        if (!task || !task->vs) {
                return -LINUX_ESRCH;
        }
        vs = task->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(vs, user_tsp, &ts, sizeof(ts));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        ms = linux_timespec_to_timeout_ms(&ts);
        if (ms < 0) {
                return ms;
        }
        return sys_poll(ufds, nfds, (i32)ms);
}
