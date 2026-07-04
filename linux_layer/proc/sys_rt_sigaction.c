#include <common/string.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_queue.h>
#include <linux_compat/signal/signal_types.h>
#include <linux_compat/signal/signal_uapi.h>
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

        if (!current || !(proc_append = linux_proc_append(current))
            || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        if (!signal_is_valid(signum) || !linux_sigsetsize_valid(sigsetsize)) {
                return -LINUX_EINVAL;
        }

        if (act_ptr != 0 && !signal_can_catch_or_ignore(signum)) {
                return -LINUX_EINVAL;
        }

        if (oldact_ptr != 0) {
                error_t e = linux_copy_sigaction_to_user(
                        vs,
                        oldact_ptr,
                        &proc_append->signal_dispositions[signum - 1]);
                i64 err = linux_mm_errno_from_copy(e);
                if (err != 0) {
                        return err;
                }
        }

        if (act_ptr != 0) {
                memset(&new_action, 0, sizeof(new_action));

                error_t e = linux_copy_sigaction_from_user(
                        vs, act_ptr, &new_action);
                i64 err = linux_mm_errno_from_copy(e);
                if (err != 0) {
                        return err;
                }

                if (new_action.sa_flags
                    & ~(SA_NOCLDSTOP | SA_NOCLDWAIT | SA_SIGINFO | SA_ONSTACK
                        | SA_RESTART | SA_NODEFER | SA_RESETHAND
                        | SA_RESTORER)) {
                        return -LINUX_EINVAL;
                }

                proc_append->signal_dispositions[signum - 1] = new_action;

                if (linux_signal_handler_is_special(new_action.sa_handler)) {
                        linux_signal_flush_pending(current, signum);
                }
        }

        return 0;
}
