#ifndef _LINUX_COMPAT_IPC_CLEAN_PROTOCOL_H_
#define _LINUX_COMPAT_IPC_CLEAN_PROTOCOL_H_

#include <common/types.h>

/*
 * clean_server IPC (linux compat):
 *   sys_exit / fatal fault -> KMSG_OP_CLEAN_THREAD_REAP (delete_thread only)
 *   wait4 reap / orphan exit -> KMSG_OP_CLEAN_TASK_REAP (delete_task only)
 * kmsg_hdr.module = clean_server_port->service_id (see doc/ai/IPC_MESSAGE.md).
 */

#define CLEAN_SERVER_PORT_NAME "clean_server_port"

#define KMSG_OP_CLEAN_THREAD_REAP  1u
#define KMSG_OP_CLEAN_TASK_REAP    2u
#define LINUX_KMSG_FMT_THREAD_REAP "p q"
#define LINUX_KMSG_FMT_TASK_REAP   "i"

#endif /* _LINUX_COMPAT_IPC_CLEAN_PROTOCOL_H_ */
