#ifndef _LINUX_COMPAT_IPC_PORT_NAMING_H_
#define _LINUX_COMPAT_IPC_PORT_NAMING_H_

#include <common/types.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/task/tcb.h>

/*
 * Canonical port-name grammar: doc/linux_compat/protocols/PORT_NAMING.md
 *
 *   {service}_listen              global listen (documented singleton)
 *   {service}_c{cpu}              per-CPU listen
 *   {service}_c{cpu}_w{wid}       worker work-port
 *   {service}_cli_{id}            client reply (usually pid)
 */

#define IPC_PORT_SERVICE_CLEAN "clean"
#define IPC_PORT_SERVICE_VFS   "vfs"

/* Well-known listen names (global singleton services). */
#define CLEAN_SERVER_PORT_NAME "clean_listen"
#define VFS_SERVER_PORT_NAME   "vfs_listen"

#define CLEAN_SERVICE_NAME IPC_PORT_SERVICE_CLEAN
#define VFS_SERVICE_NAME   IPC_PORT_SERVICE_VFS

#define CLEAN_CLIENT_PORT_PREFIX "clean_cli_"
#define VFS_CLIENT_PORT_PREFIX   "vfs_cli_"

#define CLEAN_CLIENT_PORT_NAME_MAX PORT_NAME_LEN_MAX
#define VFS_CLIENT_PORT_NAME_MAX   PORT_NAME_LEN_MAX

/*
 * Formatters: write NUL-terminated name into @buf, return length, or 0 on
 * error (buf left empty when possible).
 */
size_t ipc_port_name_listen_global(char *buf, size_t bufsize,
                                   const char *service);
size_t ipc_port_name_listen_cpu(char *buf, size_t bufsize, const char *service,
                                u32 cpu);
size_t ipc_port_name_worker(char *buf, size_t bufsize, const char *service,
                            u32 cpu, u32 wid);
size_t ipc_port_name_cli(char *buf, size_t bufsize, const char *service,
                         pid_t caller_id);

#endif /* _LINUX_COMPAT_IPC_PORT_NAMING_H_ */
