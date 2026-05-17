#include <common/string.h>
#include <linux_compat/signal/signal_init.h>
#include <linux_compat/signal/signal_types.h>

void linux_signal_init_proc_append(linux_proc_append_t* pa)
{
        int i;

        if (!pa) {
                return;
        }

        sigemptyset(&pa->pending_signals);
        for (i = 0; i < NSIG; i++) {
                pa->signal_dispositions[i].handler = SIG_DFL;
                pa->signal_dispositions[i].flags = 0;
                sigemptyset(&pa->signal_dispositions[i].mask);
                pa->signal_dispositions[i].restorer = NULL;
        }
}

void linux_signal_init_thread_append(linux_thread_append_t* ta)
{
        if (!ta) {
                return;
        }

        sigemptyset(&ta->blocked_signals);
        sigemptyset(&ta->pending_signals);
        memset(&ta->alt_stack, 0, sizeof(ta->alt_stack));
        ta->saved_main_sp = 0;
        memset(&ta->signal_restore, 0, sizeof(ta->signal_restore));
}
