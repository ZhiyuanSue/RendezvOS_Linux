#ifndef _VFS_BACKEND_IPC_H_
#define _VFS_BACKEND_IPC_H_

#include <common/types.h>
#include <linux_compat/ipc/rpc.h>
#include <rendezvos/error.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/kmsg_system.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/task/thread.h>

#include "vfs_backend.h"

/*
 * Reply ports follow PORT_NAMING: vfs_cli_k_<tag>
 *   vfs_cli_k_t<tid>       — per-thread backend RPC reply (listen + kern load)
 *   vfs_cli_k_reg_<fstype> — backend register → VFS (one per backend thread)
 * Never share one reply port across concurrent callers (preemption / SMP).
 */

#define VFS_BACKEND_IPC_OPC_FIRST (KMSG_OP_SYSTEM_END + 1u)

#define VFS_BACKEND_IPC_OPC_LOOKUP   (VFS_BACKEND_IPC_OPC_FIRST + 0u)
#define VFS_BACKEND_IPC_OPC_READ     (VFS_BACKEND_IPC_OPC_FIRST + 1u)
#define VFS_BACKEND_IPC_OPC_WRITE    (VFS_BACKEND_IPC_OPC_FIRST + 2u)
#define VFS_BACKEND_IPC_OPC_TRUNCATE (VFS_BACKEND_IPC_OPC_FIRST + 3u)
#define VFS_BACKEND_IPC_OPC_FLUSH    (VFS_BACKEND_IPC_OPC_FIRST + 4u)
#define VFS_BACKEND_IPC_OPC_READDIR  (VFS_BACKEND_IPC_OPC_FIRST + 5u)
#define VFS_BACKEND_IPC_OPC_READLINK (VFS_BACKEND_IPC_OPC_FIRST + 6u)
#define VFS_BACKEND_IPC_OPC_MKDIR    (VFS_BACKEND_IPC_OPC_FIRST + 7u)
#define VFS_BACKEND_IPC_OPC_CREATE   (VFS_BACKEND_IPC_OPC_FIRST + 8u)
#define VFS_BACKEND_IPC_OPC_UNLINK   (VFS_BACKEND_IPC_OPC_FIRST + 9u)
#define VFS_BACKEND_IPC_OPC_RENAME   (VFS_BACKEND_IPC_OPC_FIRST + 10u)
#define VFS_BACKEND_IPC_OPC_LINK     (VFS_BACKEND_IPC_OPC_FIRST + 11u)

i64 vfs_backend_ipc_call(vfs_backend_req_t *req);

/*
 * Start nested backend RPC from a coop job (port try_send request; nest-token
 * reply via transfer to vfs listen). SUCCESS → NESTED_RECV; -E_REND_AGAIN →
 * NESTED_SEND / slot busy.
 */
error_t vfs_backend_ipc_coop_nested(ipc_rpc_coop_job_t *job,
                                    vfs_backend_req_t *req);

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

/*
 * Leaf request–reply coop loop: decode via vfs_backend_ipc_rpc_handler then
 * reply. Nested callers (TLV 't' = @n<cookie>) get
 * ipc_rpc_nest_reply_transfer → vfs listen; sync callers get blocking
 * ipc_rpc_reply. Both return IPC_RPC_COOP_REPLIED.
 * @q must be unique per server thread.
 */
void vfs_backend_ipc_coop_server_loop(const char *listen_port_name,
                                      u16 service_id,
                                      ipc_rpc_coop_queue_t *q,
                                      vfs_backend_service_fn service);

/*
 * Register (+ optional mark_online when reg_flags != 0) then enter coop loop.
 * Returns <0 if register failed; otherwise does not return (blocks in loop).
 */
i64 vfs_backend_ipc_leaf_run(const char *port_name, const char *fstype,
                             u32 caps, u32 reg_flags, u16 service_id,
                             ipc_rpc_coop_queue_t *q,
                             vfs_backend_service_fn service);

/* Idempotent spawn used by DEFINE_INIT leaf backends. */
error_t vfs_backend_ipc_leaf_spawn(const char *port_name,
                                   const char *thread_name,
                                   const char *log_tag, u16 *service_id_out,
                                   Thread_Base **thread_out,
                                   void (*thread_entry)(void),
                                   bool *once_done);

#endif /* _VFS_BACKEND_IPC_H_ */
