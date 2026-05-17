#ifndef _LINUX_COMPAT_SIGNAL_DELIVER_H_
#define _LINUX_COMPAT_SIGNAL_DELIVER_H_

#include <common/stdbool.h>
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
 * @return true if trap_frame was redirected to a user signal handler
 *         (syscall return value already written; do not clobber handler regs)
 */
bool linux_deliver_pending_signals(struct trap_frame *tf);

/**
 * @brief Restore main stack pointer if returning from alternate signal stack
 *
 * This function checks if the current thread is executing on an alternate
 * signal stack and restores the main stack pointer if needed. Should be
 * called from syscall entry to detect when returning from signal handlers.
 */
void linux_restore_main_stack_if_needed(struct trap_frame *tf);

#endif /* _LINUX_COMPAT_SIGNAL_DELIVER_H_ */