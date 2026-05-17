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

static inline bool signal_is_valid(int signum)
{
        return signum >= 1 && signum <= NSIG;
}

static inline bool signal_can_catch_or_ignore(int signum)
{
        return signum != SIGKILL && signum != SIGSTOP;
}

i64 sys_rt_sigaction(i64 signum_i, u64 act_ptr, u64 oldact_ptr, u64 sigsetsize)
{
        Tcb_Base* current = get_cpu_current_task();
        linux_proc_append_t* proc_append;
        VSpace* vs;
        int signum = (int)signum_i;
        sigaction_t new_action;
        error_t e;

        if (!current || !(proc_append = linux_proc_append(current)) || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        if (!signal_is_valid(signum) || sigsetsize != sizeof(sigset_t)) {
                return -LINUX_EINVAL;
        }

        if (act_ptr != 0 && !signal_can_catch_or_ignore(signum)) {
                return -LINUX_EINVAL;
        }

        if (oldact_ptr != 0) {
                e = linux_mm_store_to_user(
                        vs, oldact_ptr,
                        &proc_append->signal_dispositions[signum - 1],
                        sizeof(sigaction_t));
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }
        }

        if (act_ptr != 0) {
                e = linux_mm_load_from_user(vs, act_ptr, &new_action,
                                            sizeof(sigaction_t));
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }

                if (new_action.flags
                    & ~(SA_NOCLDSTOP | SA_NOCLDWAIT | SA_SIGINFO | SA_ONSTACK
                        | SA_RESTART | SA_NODEFER | SA_RESETHAND | SA_RESTORER)) {
                        return -LINUX_EINVAL;
                }

                proc_append->signal_dispositions[signum - 1] = new_action;
        }

        return 0;
}
