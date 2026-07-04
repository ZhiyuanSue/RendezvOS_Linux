#include <common/stdbool.h>
#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_restore.h>
#include <linux_compat/signal/signal_types.h>
#include <modules/log/log.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/trap/trap.h>
#include <syscall.h>

/*
 * rt_sigreturn — restore user context saved when a signal handler was entered.
 *
 * Phase 2B uses kernel-side linux_signal_restore_t (not a full user
 * rt_sigframe). User handlers must call rt_sigreturn() explicitly (or via a
 * test stub).
 */

static void signal_rt_sigreturn_fatal(linux_thread_append_t* ta,
                                      const char* why)
{
        pr_warn("[SIGNAL] rt_sigreturn: %s (inflight=%u active=%u)\n",
                why,
                ta ? (unsigned)ta->signal_inflight : 0U,
                ta && ta->signal_restore.active ? 1U : 0U);
        sys_exit(128 + SIGSEGV);
        __builtin_unreachable();
}

i64 sys_rt_sigreturn(struct trap_frame* tf)
{
        Thread_Base* th = get_cpu_current_thread();
        linux_thread_append_t* ta = linux_thread_append(th);

        if (!tf) {
                return -LINUX_EINVAL;
        }

        if (!ta || ta->signal_inflight != 1 || !ta->signal_restore.active) {
                signal_rt_sigreturn_fatal(ta, "spurious");
        }

        if (!signal_restore_user_context(tf)) {
                signal_rt_sigreturn_fatal(ta, "restore failed");
        }

        return 0;
}
