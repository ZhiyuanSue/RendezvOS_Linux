#if defined(_X86_64_)

#include <arch/x86_64/tcb_arch.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_context.h>
#include <rendezvos/trap/trap.h>

void linux_signal_arch_save_context(struct trap_frame* tf,
                                    Arch_Task_Context* ctx,
                                    linux_signal_restore_t* rs)
{
        if (!tf || !rs) {
                return;
        }

        arch_syscall_get_user_return(tf,
                                     ctx,
                                     &rs->saved_user_pc,
                                     &rs->saved_user_sp,
                                     &rs->saved_syscall_ret);

        rs->arch.r15 = tf->r15;
        rs->arch.r14 = tf->r14;
        rs->arch.r13 = tf->r13;
        rs->arch.r12 = tf->r12;
        rs->arch.rbp = tf->rbp;
        rs->arch.rbx = tf->rbx;
        rs->arch.r11 = tf->r11;
        rs->arch.r10 = tf->r10;
        rs->arch.r9 = tf->r9;
        rs->arch.r8 = tf->r8;
        rs->arch.rdx = tf->rdx;
        rs->arch.rsi = tf->rsi;
        rs->arch.rdi = tf->rdi;
}

void linux_signal_arch_restore_context(struct trap_frame* tf,
                                       Arch_Task_Context* ctx,
                                       linux_signal_restore_t* rs,
                                       vaddr user_sp)
{
        if (!tf || !rs) {
                return;
        }

        tf->r15 = rs->arch.r15;
        tf->r14 = rs->arch.r14;
        tf->r13 = rs->arch.r13;
        tf->r12 = rs->arch.r12;
        tf->rbp = rs->arch.rbp;
        tf->rbx = rs->arch.rbx;
        tf->r11 = rs->arch.r11;
        tf->r10 = rs->arch.r10;
        tf->r9 = rs->arch.r9;
        tf->r8 = rs->arch.r8;
        tf->rdx = rs->arch.rdx;
        tf->rsi = rs->arch.rsi;
        tf->rdi = rs->arch.rdi;

        arch_syscall_set_user_return(
                tf, ctx, rs->saved_user_pc, user_sp, rs->saved_syscall_ret);
}

#endif /* _X86_64_ */
