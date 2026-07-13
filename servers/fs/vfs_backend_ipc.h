#ifndef _VFS_BACKEND_IPC_H_
#define _VFS_BACKEND_IPC_H_

#include <common/types.h>
#include <linux_compat/ipc/rpc.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/kmsg_system.h>
#include <rendezvos/task/tcb.h>

#include "vfs_backend.h"

#define VFS_BACKEND_IPC_CALLER_PORT "vfs_backend_caller"

#define VFS_BACKEND_IPC_OPC_FIRST (KMSG_OP_SYSTEM_END + 1u)

#define VFS_BACKEND_IPC_OPC_LOOKUP    (VFS_BACKEND_IPC_OPC_FIRST + 0u)
#define VFS_BACKEND_IPC_OPC_READ      (VFS_BACKEND_IPC_OPC_FIRST + 1u)
#define VFS_BACKEND_IPC_OPC_WRITE     (VFS_BACKEND_IPC_OPC_FIRST + 2u)
#define VFS_BACKEND_IPC_OPC_TRUNCATE  (VFS_BACKEND_IPC_OPC_FIRST + 3u)
#define VFS_BACKEND_IPC_OPC_FLUSH     (VFS_BACKEND_IPC_OPC_FIRST + 4u)
#define VFS_BACKEND_IPC_OPC_READDIR   (VFS_BACKEND_IPC_OPC_FIRST + 5u)
#define VFS_BACKEND_IPC_OPC_READLINK  (VFS_BACKEND_IPC_OPC_FIRST + 6u)
#define VFS_BACKEND_IPC_OPC_MKDIR       (VFS_BACKEND_IPC_OPC_FIRST + 7u)
#define VFS_BACKEND_IPC_OPC_CREATE      (VFS_BACKEND_IPC_OPC_FIRST + 8u)
#define VFS_BACKEND_IPC_OPC_UNLINK      (VFS_BACKEND_IPC_OPC_FIRST + 9u)
#define VFS_BACKEND_IPC_OPC_RENAME      (VFS_BACKEND_IPC_OPC_FIRST + 10u)
#define VFS_BACKEND_IPC_OPC_LINK        (VFS_BACKEND_IPC_OPC_FIRST + 11u)

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
