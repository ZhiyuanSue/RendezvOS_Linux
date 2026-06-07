#if defined(_AARCH64_)

#include <arch/aarch64/sys_ctrl.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_context.h>
#include <rendezvos/trap/trap.h>

void linux_signal_arch_save_context(struct trap_frame* tf, Arch_Task_Context* ctx,
                                    linux_signal_restore_t* rs)
{
        int i;
        u64 sp_el0 = 0;

        (void)ctx;
        if (!tf || !rs) {
                return;
        }

        for (i = 0; i < 31; i++) {
                rs->arch.regs[i] = tf->REGS[i];
        }
        rs->arch.spsr = tf->SPSR;
        rs->saved_user_pc = tf->ELR;
        rs->saved_syscall_ret = tf->REGS[0];
        mrs("SP_EL0", sp_el0);
        rs->saved_user_sp = (vaddr)sp_el0;
}

void linux_signal_arch_restore_context(struct trap_frame* tf, Arch_Task_Context* ctx,
                                       linux_signal_restore_t* rs, vaddr user_sp)
{
        int i;

        if (!tf || !rs) {
                return;
        }

        for (i = 0; i < 31; i++) {
                tf->REGS[i] = rs->arch.regs[i];
        }
        tf->ELR = rs->saved_user_pc;
        tf->SPSR = rs->arch.spsr;
        tf->REGS[0] = rs->saved_syscall_ret;
        if (ctx) {
                ctx->sp_el0 = user_sp;
        }
        msr("SP_EL0", user_sp);
}

#endif /* _AARCH64_ */
