#include <common/string.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_context.h>
#include <linux_compat/signal/signal_restore.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/trap/trap.h>

bool signal_restore_user_context(struct trap_frame* tf)
{
        Thread_Base* th = get_cpu_current_thread();
        linux_thread_append_t* ta;
        linux_signal_restore_t* rs;
        vaddr restore_sp;
        bool was_onstack;

        if (!tf || !th || !(ta = linux_thread_append(th))) {
                return false;
        }

        rs = &ta->signal_restore;
        if (!rs->active) {
                return false;
        }

        was_onstack = (ta->alt_stack.ss_flags & SS_ONSTACK) != 0;
        ta->blocked_signals = rs->saved_blocked;

        if (was_onstack) {
                ta->alt_stack.ss_flags &= ~SS_ONSTACK;
                restore_sp = ta->saved_main_sp;
        } else {
                restore_sp = rs->saved_user_sp;
        }

        linux_signal_arch_restore_context(tf, &th->ctx, rs, restore_sp);

        memset(rs, 0, sizeof(*rs));
        return true;
}
