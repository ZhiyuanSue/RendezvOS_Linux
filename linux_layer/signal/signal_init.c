#include <common/string.h>
#include <linux_compat/signal/signal_init.h>
#include <linux_compat/signal/signal_types.h>

static void linux_signal_clear_thread_signal_state(linux_thread_append_t* ta)
{
        sigemptyset(&ta->blocked_signals);
        sigemptyset(&ta->pending_signals);
        memset(&ta->alt_stack, 0, sizeof(ta->alt_stack));
        ta->saved_main_sp = 0;
        ta->signal_inflight = 0;
        memset(&ta->signal_restore, 0, sizeof(ta->signal_restore));
}

void linux_signal_init_proc_append(linux_proc_append_t* pa)
{
        int i;

        if (!pa) {
                return;
        }

        sigemptyset(&pa->pending_signals);
        for (i = 0; i < NSIG; i++) {
                pa->signal_dispositions[i].sa_handler = SIG_DFL;
                pa->signal_dispositions[i].sa_flags = 0;
                sigemptyset(&pa->signal_dispositions[i].sa_mask);
                pa->signal_dispositions[i].sa_restorer = NULL;
        }
}

void linux_signal_init_thread_append(linux_thread_append_t* ta)
{
        if (!ta) {
                return;
        }

        linux_signal_clear_thread_signal_state(ta);
}

void linux_signal_reset_thread_handler_state(linux_thread_append_t* ta)
{
        if (!ta) {
                return;
        }

        linux_signal_clear_thread_signal_state(ta);
}
