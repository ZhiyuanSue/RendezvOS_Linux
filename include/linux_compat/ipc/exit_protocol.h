#ifndef _LINUX_COMPAT_IPC_EXIT_PROTOCOL_H_
#define _LINUX_COMPAT_IPC_EXIT_PROTOCOL_H_

#include <common/types.h>
#include <rendezvos/ipc/kmsg_system.h>

/*
 * Parent wait wake after clean finishes THREAD_REAP path (Link A):
 * - Payload: child_pid + proc* + exit_code. proc* avoids parent find_proc_by_pid.
 * - Parent linux_proc_reap ref_put releases the zombie shell alloc ref.
 * - Delivery: ipc_system_try_deliver on wait_port (clean listen try+park).
 */

#define KMSG_OP_PROC_FIRST (KMSG_OP_SYSTEM_END + 1u)

#define KMSG_OP_PROC_EXIT_NOTIFY      (KMSG_OP_PROC_FIRST + 0u)
#define KMSG_OP_PROC_WAIT_INTERRUPT   (KMSG_OP_PROC_FIRST + 1u)
#define LINUX_KMSG_FMT_EXIT_NOTIFY    "q p i"
#define LINUX_KMSG_FMT_WAIT_INTERRUPT "q"

#endif /* _LINUX_COMPAT_IPC_EXIT_PROTOCOL_H_ */
