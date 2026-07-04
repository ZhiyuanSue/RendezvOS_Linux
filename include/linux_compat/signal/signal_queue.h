#ifndef _LINUX_COMPAT_SIGNAL_QUEUE_H_
#define _LINUX_COMPAT_SIGNAL_QUEUE_H_

#include <common/types.h>
#include <linux_compat/signal/signal_types.h>
#include <rendezvos/task/tcb.h>

/*
 * Core signal queuing functions (Phase 2B Layer A)
 *
 * These functions implement signal generation and queuing without
 * immediate delivery. Delivery happens in trap return path.
 */

/**
 * @brief Queue a signal for a process
 * @param target: Target process TCB
 * @param sig: Signal number (1-64)
 * @param sender_tid: Sender thread ID (for siginfo_t)
 * @return: 0 on success, negative errno on failure
 *
 * This function:
 * - Validates signal number and permissions
 * - Checks signal disposition (SIG_IGN, SIG_DFL, user handler)
 * - Adds signal to process-wide and thread-specific pending sets
 * - Wakes up target thread if necessary
 * - Handles default actions for SIG_DFL
 */
i64 linux_queue_signal(Tcb_Base *target, int sig, pid_t sender_tid);

/**
 * @brief Queue a signal for a specific thread (for tgkill)
 * @param target_thread: Target thread
 * @param sig: Signal number
 * @param sender_tid: Sender thread ID
 * @return: 0 on success, negative errno on failure
 */
i64 linux_queue_signal_thread(Thread_Base *target_thread, int sig,
                              pid_t sender_tid);

/**
 * @brief Drop a signal from process-wide and all thread pending sets.
 *
 * Called when disposition changes to SIG_IGN/SIG_DFL or when a queued signal
 * must not be delivered (Linux: ignored signals do not stay pending).
 */
void linux_signal_flush_pending(Tcb_Base *target, int sig);

#endif /* _LINUX_COMPAT_SIGNAL_QUEUE_H_ */