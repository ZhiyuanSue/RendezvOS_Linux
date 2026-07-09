#include <linux_compat/time/linux_ktime.h>
#include <linux_compat/time/linux_time_arch.h>
#include <linux_compat/initcall.h>
#include <rendezvos/task/initcall.h>

static u64 linux_boot_realtime_us;
static bool linux_time_arch_inited;

void linux_time_init_from_arch(void)
{
        if (!linux_init_bsp_once(&linux_time_arch_inited))
                return;
        linux_boot_realtime_us = linux_time_arch_boot_realtime_us();
        linux_init_bsp_mark_done(&linux_time_arch_inited);
}

u64 linux_time_monotonic_us(void)
{
        return rendezvos_time_count_to_us(rendezvos_time_now());
}

u64 linux_time_realtime_us(void)
{
        return linux_boot_realtime_us + linux_time_monotonic_us();
}

void linux_us_to_timeval(u64 us, linux_timeval_t *tv)
{
        if (!tv) {
                return;
        }
        tv->tv_sec = (linux_time_t)(us / 1000000ull);
        tv->tv_usec = (linux_suseconds_t)(us % 1000000ull);
}

void linux_us_to_timespec(u64 us, linux_timespec_t *ts)
{
        if (!ts) {
                return;
        }
        ts->tv_sec = (linux_time_t)(us / 1000000ull);
        ts->tv_nsec = (i64)((us % 1000000ull) * 1000ull);
}

void linux_ticks_to_timespec(tick_t rem, linux_timespec_t *ts)
{
        u64 us;

        if (!ts) {
                return;
        }
        us = rendezvos_time_count_to_us(rem);
        linux_us_to_timespec(us, ts);
}

static u64 linux_timespec_to_us(const linux_timespec_t *ts)
{
        u64 total_ns = (u64)ts->tv_sec * 1000000000ull + (u64)ts->tv_nsec;
        u64 us = total_ns / 1000ull;

        if (total_ns % 1000ull != 0) {
                us += 1;
        }
        return us;
}

tick_t linux_time_resolve_deadline(i32 clockid, const linux_timespec_t *ts,
                                   bool absolute, bool *already_satisfied)
{
        tick_t now = rendezvos_time_now();

        if (already_satisfied) {
                *already_satisfied = false;
        }

        if (!absolute) {
                return now
                       + rendezvos_time_us_to_count(linux_timespec_to_us(ts));
        }

        if (clockid == LINUX_CLOCK_MONOTONIC) {
                tick_t deadline =
                        rendezvos_time_us_to_count(linux_timespec_to_us(ts));

                if (!time_before(now, deadline)) {
                        if (already_satisfied) {
                                *already_satisfied = true;
                        }
                        return now;
                }
                return deadline;
        }

        {
                u64 target_us = linux_timespec_to_us(ts);
                u64 current_us = linux_time_realtime_us();

                if (target_us <= current_us) {
                        if (already_satisfied) {
                                *already_satisfied = true;
                        }
                        return now;
                }
                return now + rendezvos_time_us_to_count(target_us - current_us);
        }
}

DEFINE_INIT_LEVEL(linux_time_init_from_arch, 2);
