#include <common/string.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_restore.h>
#include <modules/log/log.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/trap/trap.h>

bool signal_restore_user_context(struct trap_frame* tf)
{
        Thread_Base* th = get_cpu_current_thread();
        linux_thread_append_t* ta;
        linux_signal_restore_t* rs;

        if (!tf || !th || !(ta = linux_thread_append(th))) {
                return false;
        }

        rs = &ta->signal_restore;
        if (!rs->active) {
                pr_debug("[SIGNAL] rt_sigreturn: no saved handler context\n");
                return false;
        }

        vaddr restore_sp;
        bool was_onstack = (ta->alt_stack.ss_flags & SS_ONSTACK) != 0;

        ta->blocked_signals = rs->saved_blocked;

        if (was_onstack) {
                ta->alt_stack.ss_flags &= ~SS_ONSTACK;
                restore_sp = ta->saved_main_sp;
        } else {
                restore_sp = rs->saved_user_sp;
        }

        arch_syscall_set_user_return(tf, &th->ctx, rs->saved_user_pc, restore_sp,
                                     rs->saved_syscall_ret);

        memset(rs, 0, sizeof(*rs));
        pr_info("[SIGNAL] rt_sigreturn restored user context\n");
        return true;
}
