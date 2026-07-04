#ifndef _LINUX_COMPAT_IPC_CLEAN_PROTOCOL_H_
#define _LINUX_COMPAT_IPC_CLEAN_PROTOCOL_H_

#include <common/types.h>

/*
 * clean_server IPC (linux compat): sys_exit -> clean_server thread reap.
 * kmsg_hdr.module = clean_server_port->service_id (see doc/ai/IPC_MESSAGE.md).
 */

#define CLEAN_SERVER_PORT_NAME "clean_server_port"

#define KMSG_OP_CLEAN_THREAD_REAP  1u
#define LINUX_KMSG_FMT_THREAD_REAP "p q"

#endif /* _LINUX_COMPAT_IPC_CLEAN_PROTOCOL_H_ */
