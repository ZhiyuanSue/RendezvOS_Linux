#ifndef _LINUX_COMPAT_IPC_EXIT_PROTOCOL_H_
#define _LINUX_COMPAT_IPC_EXIT_PROTOCOL_H_

#include <common/types.h>

/*
 * Parent wait4 wakeup: child sys_exit -> parent proc wait_port.
 * kmsg_hdr.module = parent wait_port->service_id.
 * Payload is a wakeup hook only; reaping uses exit_state (see sys_wait.c).
 */

#define KMSG_OP_PROC_EXIT_NOTIFY 1u
#define LINUX_KMSG_FMT_EXIT_NOTIFY "qi"

#endif /* _LINUX_COMPAT_IPC_EXIT_PROTOCOL_H_ */
