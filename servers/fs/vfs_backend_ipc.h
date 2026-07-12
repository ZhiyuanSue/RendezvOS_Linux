#ifndef _VFS_BACKEND_IPC_H_
#define _VFS_BACKEND_IPC_H_

#include <common/types.h>
#include <linux_compat/ipc/rpc.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/task/tcb.h>

#include "vfs_backend.h"

#define VFS_BACKEND_IPC_CALLER_PORT "vfs_backend_caller"

#define VFS_BACKEND_IPC_OPC_LOOKUP   1u
#define VFS_BACKEND_IPC_OPC_READ      2u
#define VFS_BACKEND_IPC_OPC_WRITE     3u
#define VFS_BACKEND_IPC_OPC_TRUNCATE  4u
#define VFS_BACKEND_IPC_OPC_FLUSH     5u

typedef i64 (*vfs_backend_service_fn)(vfs_backend_req_t *req);

i64 vfs_backend_ipc_call(vfs_backend_req_t *req);

i64 vfs_backend_ipc_rpc_handler(u16 opcode, const kmsg_t *km,
                                char **reply_port_out,
                                vfs_backend_service_fn service);

error_t vfs_backend_ipc_server_spawn(const char *port_name,
                                     const char *thread_name,
                                     u16 *service_id_out,
                                     Thread_Base **thread_out,
                                     void (*thread_entry)(void));

i64 vfs_backend_ipc_register(const char *port_name, const char *fstype,
                             u32 caps, u32 reg_flags);

#endif /* _VFS_BACKEND_IPC_H_ */
