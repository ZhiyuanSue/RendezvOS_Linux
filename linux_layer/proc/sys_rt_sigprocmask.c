#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_state.h>
#include <linux_compat/signal/signal_types.h>
#include <linux_compat/signal/signal_uapi.h>
#include <rendezvos/error.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

static void signal_mask_sanitize_helper(sigset_t* set)
{
        sigdelset(set, SIGKILL);
        sigdelset(set, SIGSTOP);
}

i64 sys_rt_sigprocmask(i64 how_i, u64 set_ptr, u64 oldset_ptr, u64 sigsetsize)
{
        Thread_Base* current_thread = get_cpu_current_thread();
        Tcb_Base* current = get_cpu_current_task();
        linux_signal_thread_state_t* ts;
        VSpace* vs;
        int how = (int)how_i;
        sigset_t new_set;
        error_t e;

        if (!current_thread || !current || !current->vs) {
                return -LINUX_ESRCH;
        }

        ts = linux_signal_thread_state(current_thread);
        if (!ts) {
                return -LINUX_ENOMEM;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        if (!linux_sigsetsize_valid(sigsetsize)) {
                return -LINUX_EINVAL;
        }

        if (oldset_ptr != 0) {
                e = linux_mm_store_to_user(vs,
                                           oldset_ptr,
                                           &ts->blocked_signals,
                                           sizeof(sigset_t));
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }
        }

        if (set_ptr == 0) {
                return 0;
        }

        e = linux_mm_load_from_user(vs, set_ptr, &new_set, sizeof(sigset_t));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) {
                return -LINUX_EINVAL;
        }

        switch (how) {
        case SIG_BLOCK:
                for (int i = 0; i < (int)(64 / (8 * sizeof(unsigned long)));
                     i++) {
                        ts->blocked_signals.sig[i] |= new_set.sig[i];
                }
                break;
        case SIG_UNBLOCK:
                for (int i = 0; i < (int)(64 / (8 * sizeof(unsigned long)));
                     i++) {
                        ts->blocked_signals.sig[i] &=
                                ~new_set.sig[i];
                }
                break;
        case SIG_SETMASK:
                ts->blocked_signals = new_set;
                break;
        }

        signal_mask_sanitize_helper(&ts->blocked_signals);
        return 0;
}
