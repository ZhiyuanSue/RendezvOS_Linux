#ifndef _LINUX_COMPAT_IPC_CLEAN_PROTOCOL_H_
#define _LINUX_COMPAT_IPC_CLEAN_PROTOCOL_H_

#include <common/types.h>
#include <linux_compat/ipc/port_naming.h>
#include <rendezvos/ipc/kmsg_system.h>

/*
 * clean_server IPC — doc/linux_compat/protocols/EXIT_CLEAN.md (v2)
 *   Global listen: CLEAN_SERVER_PORT_NAME ("clean_listen")
 *   One-way message: THREAD_REAP only (exitor → listen).
 */

#define KMSG_OP_CLEAN_THREAD_REAP (KMSG_OP_SYSTEM_END + 1u)
#define LINUX_KMSG_FMT_THREAD_REAP "p q"

#endif /* _LINUX_COMPAT_IPC_CLEAN_PROTOCOL_H_ */
