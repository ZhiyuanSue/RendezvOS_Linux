#include <common/string.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_restore.h>
#include <linux_compat/signal/signal_stack.h>
#include <modules/log/log.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/trap/trap.h>

#if defined(_X86_64_)
#include <arch/x86_64/boot/arch_setup.h>
#elif defined(_AARCH64_)
#include <arch/aarch64/sys_ctrl.h>
#include <arch/aarch64/tcb_arch.h>
#endif

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

        signal_restore_user_sp(ta, tf, restore_sp);

#if defined(_X86_64_)
        tf->rcx = rs->saved_user_pc;
        tf->rax = rs->saved_syscall_ret;
#elif defined(_AARCH64_)
        tf->ELR = rs->saved_user_pc;
        tf->REGS[0] = rs->saved_syscall_ret;
#endif

        memset(rs, 0, sizeof(*rs));
        pr_info("[SIGNAL] rt_sigreturn restored user context\n");
        return true;
}
