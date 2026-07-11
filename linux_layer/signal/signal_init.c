#include <linux_compat/signal/signal_init.h>
#include <linux_compat/signal/signal_state.h>

void linux_signal_init_proc_append(linux_proc_append_t* pa)
{
        if (pa && pa->signal) {
                linux_signal_reinit_proc_state(pa->signal);
        }
}

void linux_signal_init_thread_append(linux_thread_append_t* ta)
{
        if (ta && ta->signal) {
                linux_signal_reinit_thread_state(ta->signal);
        }
}

void linux_signal_reset_thread_handler_state(linux_thread_append_t* ta)
{
        if (!ta || !ta->signal) {
                return;
        }

        linux_signal_reinit_thread_state(ta->signal);
}
