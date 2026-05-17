#include <common/stdbool.h>
#include <linux_compat/errno.h>
#include <linux_compat/signal/signal_restore.h>
#include <modules/log/log.h>
#include <rendezvos/trap/trap.h>
#include <syscall.h>

/*
 * rt_sigreturn — restore user context saved when a signal handler was entered.
 *
 * Phase 2B uses kernel-side linux_signal_restore_t (not a full user rt_sigframe).
 * User handlers must call rt_sigreturn() explicitly (or via a test stub).
 */

i64 sys_rt_sigreturn(struct trap_frame* tf)
{
        if (!tf) {
                return -LINUX_EINVAL;
        }

        if (!signal_restore_user_context(tf)) {
                pr_warn("[SIGNAL] rt_sigreturn: nothing to restore\n");
                return -LINUX_EINVAL;
        }

        return 0;
}
