#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_stack.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>

#if defined(_X86_64_)
#include <arch/x86_64/boot/arch_setup.h>
#elif defined(_AARCH64_)
#include <arch/aarch64/sys_ctrl.h>
#include <arch/aarch64/tcb_arch.h>
#endif

void signal_set_user_sp(linux_thread_append_t* ta, struct trap_frame* tf,
                        vaddr user_sp)
{
        Thread_Base* th = get_cpu_current_thread();

        (void)ta;
#if defined(_X86_64_)
        (void)th;  /* Unused on x86_64 */
        percpu(user_rsp_scratch) = user_sp;
        if (tf) {
                (void)tf;
        }
#elif defined(_AARCH64_)
        if (tf) {
                tf->SP = user_sp;
        }
        if (th) {
                arch_set_thread_user_sp(&th->ctx, user_sp);
        }
        msr("SP_EL0", user_sp);
#endif
}

void signal_restore_user_sp(linux_thread_append_t* ta, struct trap_frame* tf,
                            vaddr user_sp)
{
        (void)ta;
        signal_set_user_sp(ta, tf, user_sp);
}
