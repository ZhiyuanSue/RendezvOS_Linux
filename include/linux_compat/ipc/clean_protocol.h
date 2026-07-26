#ifndef _LINUX_COMPAT_IPC_CLEAN_PROTOCOL_H_
#define _LINUX_COMPAT_IPC_CLEAN_PROTOCOL_H_

#include <common/types.h>
#include <linux_compat/ipc/port_naming.h>
#include <rendezvos/ipc/kmsg_system.h>

/*
 * clean_server IPC — full protocol:
 *   doc/linux_compat/protocols/EXIT_CLEAN.md
 * Port names: doc/linux_compat/protocols/PORT_NAMING.md
 *   listen  = CLEAN_SERVER_PORT_NAME ("clean_listen")
 *   workers = clean_c{cpu}_w{wid}
 *   client  = clean_cli_{pid}
 *
 * kmsg_hdr.module = clean listen port->service_id
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
