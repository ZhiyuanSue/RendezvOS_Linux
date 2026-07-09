#ifndef _LINUX_COMPAT_SIGNAL_DELIVER_H_
#define _LINUX_COMPAT_SIGNAL_DELIVER_H_

#include <common/stdbool.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/trap/trap.h>

/*
 * Signal delivery function (Phase 2B Layer B)
 *
 * This function is called from syscall return path to deliver
 * pending signals before returning to user space.
 */

/**
 * @brief Deliver pending signals to current thread
 * @param tf: Current trap frame (may be modified to return to signal handler)
 *
 * This function:
 * - Checks if current thread has pending signals
 * - Selects highest priority signal to deliver
 * - Handles signal disposition (SIG_DFL, SIG_IGN, user handler)
 * - Builds signal frame on user stack (if user handler)
 * - Modifies trap_frame to return to signal handler (if user handler)
 * - Clears delivered signal from pending set
 *
 * Called from syscall_entry.c before returning to user space.
 */
/**
 * @return true if current thread has a signal that would be delivered at
 *         syscall return (used by sleep paths for EINTR).
 */
bool linux_signal_has_deliverable_pending(void);

bool linux_signal_thread_has_deliverable_pending(Thread_Base *thread);

/**
 * @return true if a pending signal should interrupt wait4/nanosleep with EINTR.
 *         SIGCHLD with SIG_DFL/SIG_IGN does not interrupt wait4 (Linux
 * semantics).
 */
bool linux_signal_wait4_should_return_eintr(Thread_Base *thread);

/**
 * @return true if trap_frame was redirected to a user signal handler
 *         (syscall return value already written; do not clobber handler regs)
 */
bool linux_deliver_pending_signals(struct trap_frame *tf);

#endif /* _LINUX_COMPAT_SIGNAL_DELIVER_H_ */