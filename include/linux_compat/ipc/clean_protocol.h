#ifndef _LINUX_COMPAT_IPC_CLEAN_PROTOCOL_H_
#define _LINUX_COMPAT_IPC_CLEAN_PROTOCOL_H_

#include <common/types.h>
#include <linux_compat/ipc/port_naming.h>
#include <rendezvos/ipc/kmsg_system.h>

/*
 * clean_server IPC — full protocol:
 *   doc/linux_compat/protocols/EXIT_CLEAN.md
 * Port names: doc/linux_compat/protocols/PORT_NAMING.md §4
 *   listen = CLEAN_SERVER_PORT_NAME ("clean_listen") — one global port
 *   threads = one clean_server per CPU, all recv the same port
 *   client = clean_cli_{pid}
 *   (no worker work-ports; EXIT_NOTIFY is try+park on listen)
 *
 * kmsg_hdr.module = clean_listen port->service_id
 * Cross-CPU: core0's THREAD_REAP may be handled by core1's clean thread.
 */

#define KMSG_OP_CLEAN_FIRST (KMSG_OP_SYSTEM_END + 1u)

#define KMSG_OP_CLEAN_THREAD_REAP    (KMSG_OP_CLEAN_FIRST + 0u)
#define KMSG_OP_CLEAN_TASK_REAP      (KMSG_OP_CLEAN_FIRST + 1u)
#define KMSG_OP_CLEAN_TASK_REAP_SYNC (KMSG_OP_CLEAN_FIRST + 2u)

#define LINUX_KMSG_FMT_THREAD_REAP "p q"
#define LINUX_KMSG_FMT_TASK_REAP   "i"
/* SYNC wire: business "i" + ipc_rpc_call appends 't' → decode "it" */
#define LINUX_KMSG_FMT_TASK_REAP_SYNC "it"

#endif /* _LINUX_COMPAT_IPC_CLEAN_PROTOCOL_H_ */
