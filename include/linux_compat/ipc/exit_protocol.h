#ifndef _LINUX_COMPAT_IPC_EXIT_PROTOCOL_H_
#define _LINUX_COMPAT_IPC_EXIT_PROTOCOL_H_

#include <common/types.h>
#include <rendezvos/ipc/kmsg_system.h>

/*
 * Zombie wakeup after THREAD_REAP (thread_number==0):
 * - Live parent: blocking send_msg on wait_port_<ppid> (from a clean_server
 *   per-message worker so the listen loop is not stalled).
 * - Reparented / dead parent: send_msg on kernel_port (init thread recv).
 * kmsg_hdr.module = target port service_id; payload carries child_pid + exit_code.
 */

#define KMSG_OP_PROC_FIRST (KMSG_OP_SYSTEM_END + 1u)

#define KMSG_OP_PROC_EXIT_NOTIFY      (KMSG_OP_PROC_FIRST + 0u)
#define KMSG_OP_PROC_WAIT_INTERRUPT   (KMSG_OP_PROC_FIRST + 1u)
#define LINUX_KMSG_FMT_EXIT_NOTIFY    "qi"
#define LINUX_KMSG_FMT_WAIT_INTERRUPT "q"

#endif /* _LINUX_COMPAT_IPC_EXIT_PROTOCOL_H_ */
