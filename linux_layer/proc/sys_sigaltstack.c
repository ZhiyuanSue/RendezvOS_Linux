#include <common/string.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_types.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

static int signal_user_stack_access_helper(u64 user_ptr, stack_t* kstack,
                                           bool to_user, VSpace* vs)
{
        error_t e;

        if (user_ptr == 0) {
                return 0;
        }
        if (!kstack || !vs || !linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }
        if ((user_ptr & (PAGE_SIZE - 1)) + sizeof(stack_t) > PAGE_SIZE) {
                return -LINUX_EFAULT;
        }

        if (to_user) {
                e = linux_mm_store_to_user(vs, user_ptr, kstack, sizeof(stack_t));
        } else {
                e = linux_mm_load_from_user(vs, user_ptr, kstack, sizeof(stack_t));
        }
        return (e == REND_SUCCESS) ? 0 : -LINUX_EFAULT;
}

i64 sys_sigaltstack(u64 ss_ptr, u64 old_ss_ptr)
{
        Thread_Base* current_thread = get_cpu_current_thread();
        Tcb_Base* process;
        linux_thread_append_t* thread_append;
        VSpace* vs;

        if (!current_thread
            || !(thread_append = linux_thread_append(current_thread))
            || !(process = current_thread->belong_tcb) || !process->vs) {
                return -LINUX_ESRCH;
        }

        vs = process->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        if (old_ss_ptr != 0) {
                stack_t old_stack;

                memcpy(&old_stack, &thread_append->alt_stack, sizeof(old_stack));
                if (old_stack.ss_sp == NULL) {
                        old_stack.ss_flags = SS_DISABLE;
                }
                if (signal_user_stack_access_helper(
                            old_ss_ptr, &old_stack, true, vs)
                    != 0) {
                        return -LINUX_EFAULT;
                }
        }

        if (ss_ptr != 0) {
                stack_t new_stack;

                if (signal_user_stack_access_helper(
                            ss_ptr, &new_stack, false, vs)
                    != 0) {
                        return -LINUX_EFAULT;
                }

                if (thread_append->alt_stack.ss_flags & SS_ONSTACK) {
                        return -LINUX_EPERM;
                }

                if (new_stack.ss_flags & SS_DISABLE) {
                        memset(&thread_append->alt_stack, 0,
                               sizeof(thread_append->alt_stack));
                        thread_append->alt_stack.ss_flags = SS_DISABLE;
                        return 0;
                }

                if (new_stack.ss_size < MINSIGSTKSZ) {
                        return -LINUX_ENOMEM;
                }

                memcpy(&thread_append->alt_stack, &new_stack, sizeof(new_stack));
        }

        return 0;
}
