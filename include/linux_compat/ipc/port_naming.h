#ifndef _LINUX_COMPAT_IPC_PORT_NAMING_H_
#define _LINUX_COMPAT_IPC_PORT_NAMING_H_

#include <common/types.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/smp/cpu_id.h>
#include <rendezvos/task/tcb.h>

/*
 * Canonical port-name grammar: doc/linux_compat/protocols/PORT_NAMING.md
 *
 * Live names:
 *   {service}_listen     global listen (clean_listen, vfs_listen)
 *   {service}_c{cpu}     per-CPU listen (when a service chooses §3.1)
 *   {service}_cli_{id}   client reply (usually pid; vfs_cli_k_* for kern)
 *
 * clean: §4 global listen — all per-CPU clean threads recv the same port.
 */

#define IPC_PORT_SERVICE_CLEAN "clean"
#define IPC_PORT_SERVICE_VFS   "vfs"

#define CLEAN_SERVER_PORT_NAME "clean_listen"
#define VFS_SERVER_PORT_NAME   "vfs_listen"

#define CLEAN_SERVICE_NAME IPC_PORT_SERVICE_CLEAN
#define VFS_SERVICE_NAME   IPC_PORT_SERVICE_VFS

#define CLEAN_CLIENT_PORT_PREFIX "clean_cli_"
#define VFS_CLIENT_PORT_PREFIX   "vfs_cli_"

#define CLEAN_CLIENT_PORT_NAME_MAX PORT_NAME_LEN_MAX
#define VFS_CLIENT_PORT_NAME_MAX   PORT_NAME_LEN_MAX

/* "{service}_cli_{caller_id}" — length or 0 on error. */
size_t ipc_port_name_cli(char *buf, size_t bufsize, const char *service,
                         pid_t caller_id);

/* "{service}_c{cpu}" — per-CPU listen; length or 0 on error. */
size_t ipc_port_name_listen_cpu(char *buf, size_t bufsize, const char *service,
                                cpu_id_t cpu);

#endif /* _LINUX_COMPAT_IPC_PORT_NAMING_H_ */
