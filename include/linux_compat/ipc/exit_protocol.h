#ifndef _LINUX_COMPAT_IPC_EXIT_PROTOCOL_H_
#define _LINUX_COMPAT_IPC_EXIT_PROTOCOL_H_

#include <common/types.h>

/*
 * Zombie wakeup after THREAD_REAP (thread_number==0):
 * - Live parent: send_msg on wait_port_<ppid> (wait4 recv).
 * - Reparented / dead parent: send_msg on kernel_port (init thread recv).
 * kmsg_hdr.module = target port service_id; payload is a wakeup hint only.
 */

#define KMSG_OP_PROC_EXIT_NOTIFY      1u
#define KMSG_OP_PROC_WAIT_INTERRUPT   2u
#define LINUX_KMSG_FMT_EXIT_NOTIFY    "qi"
#define LINUX_KMSG_FMT_WAIT_INTERRUPT "q"

#endif /* _LINUX_COMPAT_IPC_EXIT_PROTOCOL_H_ */
