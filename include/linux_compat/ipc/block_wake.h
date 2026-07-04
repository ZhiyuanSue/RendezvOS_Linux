#ifndef _LINUX_COMPAT_IPC_BLOCK_WAKE_H_
#define _LINUX_COMPAT_IPC_BLOCK_WAKE_H_

#include <rendezvos/ipc/port.h>
#include <rendezvos/task/tcb.h>

/*
 * Generic recv_msg block interrupt (RPC reply ports, etc.).
 * kmsg_hdr.module = destination port service_id.
 * Distinct from KMSG_OP_PROC_WAIT_INTERRUPT (wait4) and timer CANCEL (sleep).
 */

#define KMSG_OP_IPC_RECV_INTERRUPT        1u
#define LINUX_KMSG_FMT_IPC_RECV_INTERRUPT "q"

bool linux_ipc_post_recv_interrupt(Message_Port_t *port);
void linux_ipc_recv_wake_for_signal(Thread_Base *thread, Message_Port_t *port);

#endif
