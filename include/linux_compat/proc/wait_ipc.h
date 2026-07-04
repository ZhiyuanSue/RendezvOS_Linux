#ifndef _LINUX_COMPAT_PROC_WAIT_IPC_H_
#define _LINUX_COMPAT_PROC_WAIT_IPC_H_

#include <rendezvos/task/tcb.h>

/*
 * wait4 IPC wake helpers (linux_layer/proc/proc_wait_ipc.c).
 * Child exit uses KMSG_OP_PROC_EXIT_NOTIFY; signal EINTR uses WAIT_INTERRUPT.
 */

void linux_proc_wait_wake_for_signal(Thread_Base *thread, Tcb_Base *process);

#endif
