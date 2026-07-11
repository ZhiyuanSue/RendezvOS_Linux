#include <common/string.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_context.h>
#include <linux_compat/signal/signal_restore.h>
#include <linux_compat/signal/signal_state.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/trap/trap.h>

bool signal_restore_user_context(struct trap_frame* tf)
{
        Thread_Base* th = get_cpu_current_thread();
        linux_signal_thread_state_t* ts;
        linux_signal_restore_t* rs;
        vaddr restore_sp;
        bool was_onstack;

        if (!tf || !th) {
                return false;
        }

        ts = linux_signal_thread_state(th);
        if (!ts) {
                return false;
        }

        rs = &ts->signal_restore;
        if (!rs->active || ts->signal_inflight != 1) {
                return false;
        }

        was_onstack = (ts->alt_stack.ss_flags & SS_ONSTACK) != 0;
        ts->blocked_signals = rs->saved_blocked;

        if (was_onstack) {
                ts->alt_stack.ss_flags &= ~SS_ONSTACK;
                restore_sp = ts->saved_main_sp;
        } else {
                restore_sp = rs->saved_user_sp;
        }

        linux_signal_arch_restore_context(tf, &th->ctx, rs, restore_sp);

        memset(rs, 0, sizeof(*rs));
        ts->signal_inflight = 0;
        return true;
}
