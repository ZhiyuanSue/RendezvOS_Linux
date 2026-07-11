#include <common/mm.h>
#include <common/stddef.h>
#include <common/string.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/signal/signal_state.h>
#include <linux_compat/signal/signal_types.h>
#include <linux_compat/signal/signal_altstack.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

/*
 * Copy stack_t field-by-field so a user stack_t near a page boundary does not
 * fail the old single-chunk "must fit in one page" check.
 */
static int signal_copy_stack_t_from_user(VSpace *vs, u64 user_ptr, stack_t *out)
{
        error_t e;

        if (!out || !vs) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(
                vs, user_ptr, &out->ss_sp, sizeof(out->ss_sp));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        e = linux_mm_load_from_user(vs,
                                    user_ptr + offsetof(stack_t, ss_flags),
                                    &out->ss_flags,
                                    sizeof(out->ss_flags));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        e = linux_mm_load_from_user(vs,
                                    user_ptr + offsetof(stack_t, ss_size),
                                    &out->ss_size,
                                    sizeof(out->ss_size));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        return 0;
}

static int signal_copy_stack_t_to_user(VSpace *vs, u64 user_ptr,
                                       const stack_t *kstack)
{
        error_t e;

        if (!kstack || !vs) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_store_to_user(
                vs, user_ptr, &kstack->ss_sp, sizeof(kstack->ss_sp));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        e = linux_mm_store_to_user(vs,
                                   user_ptr + offsetof(stack_t, ss_flags),
                                   &kstack->ss_flags,
                                   sizeof(kstack->ss_flags));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        e = linux_mm_store_to_user(vs,
                                   user_ptr + offsetof(stack_t, ss_size),
                                   &kstack->ss_size,
                                   sizeof(kstack->ss_size));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        return 0;
}

static int signal_validate_new_altstack(const stack_t *new_stack)
{
        if (new_stack->ss_flags & SS_ONSTACK) {
                return -LINUX_EINVAL;
        }
        if (new_stack->ss_flags & ~(SS_DISABLE | SS_AUTODISARM)) {
                return -LINUX_EINVAL;
        }
        if (new_stack->ss_flags & SS_DISABLE) {
                return 0;
        }
        if (new_stack->ss_sp == NULL || new_stack->ss_size == 0) {
                return -LINUX_EINVAL;
        }
        if (new_stack->ss_size < MINSIGSTKSZ) {
                return -LINUX_ENOMEM;
        }
        /*
         * Linux does not probe alt-stack memory at sigaltstack() time; defer
         * mapping checks to signal delivery (SA_ONSTACK).
         */
        return 0;
}

i64 sys_sigaltstack(u64 ss_ptr, u64 old_ss_ptr)
{
        Thread_Base *current_thread = get_cpu_current_thread();
        Tcb_Base *process;
        linux_signal_thread_state_t *ts;
        VSpace *vs;

        if (!current_thread || !(process = current_thread->belong_tcb)
            || !process->vs) {
                return -LINUX_ESRCH;
        }

        ts = linux_signal_thread_state(current_thread);
        if (!ts) {
                return -LINUX_ENOMEM;
        }

        vs = process->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        if (old_ss_ptr != 0) {
                stack_t old_stack;

                memcpy(&old_stack, &ts->alt_stack, sizeof(old_stack));
                if (old_stack.ss_sp == NULL
                    || (old_stack.ss_flags & SS_DISABLE)) {
                        old_stack.ss_sp = NULL;
                        old_stack.ss_size = 0;
                        old_stack.ss_flags = SS_DISABLE;
                }
                if (signal_copy_stack_t_to_user(vs, old_ss_ptr, &old_stack)
                    != 0) {
                        return -LINUX_EFAULT;
                }
        }

        if (ss_ptr != 0) {
                stack_t new_stack;
                int ret;

                if (ts->alt_stack.ss_flags & SS_ONSTACK) {
                        return -LINUX_EPERM;
                }

                ret = signal_copy_stack_t_from_user(vs, ss_ptr, &new_stack);
                if (ret != 0) {
                        return ret;
                }

                ret = signal_validate_new_altstack(&new_stack);
                if (ret != 0) {
                        pr_debug(
                                "[SIGNAL] sigaltstack reject flags=0x%x size=%lu sp=%p ret=%d\n",
                                new_stack.ss_flags,
                                (unsigned long)new_stack.ss_size,
                                new_stack.ss_sp,
                                ret);
                        return ret;
                }

                if (new_stack.ss_flags & SS_DISABLE) {
                        memset(&ts->alt_stack, 0, sizeof(ts->alt_stack));
                        ts->alt_stack.ss_flags = SS_DISABLE;
                        return 0;
                }

                new_stack.ss_flags &= ~SS_ONSTACK;
                memcpy(&ts->alt_stack, &new_stack, sizeof(new_stack));
        }

        return 0;
}
