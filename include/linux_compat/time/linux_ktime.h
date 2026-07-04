#ifndef _LINUX_COMPAT_KTIME_H_
#define _LINUX_COMPAT_KTIME_H_

#include <common/types.h>
#include <linux_compat/time/linux_time_types.h>
#include <rendezvos/time.h>

/* Monotonic microseconds since boot. */
u64 linux_time_monotonic_us(void);

/* Wall-clock microseconds since Unix epoch (coarse; see linux_ktime.c). */
u64 linux_time_realtime_us(void);

void linux_time_init_from_arch(void);

void linux_us_to_timeval(u64 us, linux_timeval_t *tv);
void linux_us_to_timespec(u64 us, linux_timespec_t *ts);
void linux_ticks_to_timespec(tick_t rem, linux_timespec_t *ts);

/*
 * Resolve a sleep request to a monotonic deadline (rendezvos_time_now domain).
 * @p already_satisfied is set when absolute target is in the past (Linux:
 * return 0).
 */
tick_t linux_time_resolve_deadline(i32 clockid, const linux_timespec_t *ts,
                                   bool absolute, bool *already_satisfied);

/*
 * Block until monotonic time reaches @p deadline_count (rendezvos_time_now
 * domain). Uses rendezvos_timer_event + per-thread sleep IPC port (see
 * linux_time_sleep.c). Returns 0 on success, -LINUX_EINTR if interrupted
 * (signal wake posts CANCEL to sleep_port; see
 * linux_time_sleep_wake_for_signal).
 */
i64 linux_time_sleep_until_count(tick_t deadline_count,
                                 linux_timespec_t *rem_out);

#endif
