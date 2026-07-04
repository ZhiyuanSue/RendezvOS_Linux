#ifndef _LINUX_COMPAT_TIME_SLEEP_H_
#define _LINUX_COMPAT_TIME_SLEEP_H_

#include <rendezvos/task/tcb.h>

/*
 * Per-thread sleep IPC port (linux_layer/time/linux_time_sleep.c).
 * Timer IRQ and signal wake both deliver kmsg to this port to unblock recv_msg.
 */

void linux_time_sleep_wake_for_signal(Thread_Base *thread);
void linux_time_sleep_port_teardown(Thread_Base *thread);

#endif
