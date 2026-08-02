#ifndef _LINUX_COMPAT_IPC_RPC_H_
#define _LINUX_COMPAT_IPC_RPC_H_

#include <common/stdarg.h>
#include <common/stdbool.h>
#include <common/types.h>
#include <linux_compat/ipc/port_naming.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/task/tcb.h>

/*
 * IPC helpers on core send_msg/recv_msg + kmsg TLV (reply port = 't').
 *
 * Listen models (no per-message OS worker pool — do not reintroduce):
 *   ipc_server_coop_loop  — preferred: try_recv + poll parked work
 *   ipc_rpc_server_loop   — transitional: same-thread blocking
 *                           recv → handler → send_msg(reply)
 *                           (VFS/backends today; not a thread pool)
 */

#define IPC_RPC_RESP_OPCODE_DEFAULT 0u
#define IPC_RPC_RESP_FMT_DEFAULT    "q"

/* Scan ipc_serial TLV payload for the last 't' (reply port name). */
const char* ipc_serial_payload_reply_port(const u8* payload, u32 len);

void ipc_rpc_drain_recv_queue(void);

/*
 * Format "<prefix><pid>" into buf (uses proc_format_pid). Returns total length
 * or 0 on error.
 */
size_t ipc_rpc_format_port_name(char* buf, size_t bufsize, const char* prefix,
                                pid_t pid);

/*
 * Lookup globally registered port, or create + register on global_port_table.
 */
Message_Port_t* ipc_rpc_port_lookup_or_create(const char* port_name);

/*
 * Remove a globally registered RPC client port by name prefix + pid
 * (idempotent).
 */
void ipc_rpc_unregister_port_by_pid(const char* prefix, pid_t pid);

/* Remove one registered port by exact name (idempotent). */
void ipc_rpc_unregister_port_name(const char* port_name);

/*
 * Blocking RPC: variadic args match @req_fmt; reply port TLV 't' appended.
 * Response uses @resp_opcode + @resp_fmt (VFS passes KMSG_OP_VFS_RESP / "q").
 *
 * Interruptible: -EINTR only if a deliverable signal is pending *before*
 * send_msg(server). After the request is committed, the call always waits for
 * the reply (or reply-port close); IPC_RECV_INTERRUPT is drained, never used
 * to abandon recv (that wedges single-threaded send_msg(reply) servers).
 */
i64 ipc_rpc_call_va(Message_Port_t* server_port, Message_Port_t* reply_port,
                    u16 req_opcode, const char* req_fmt, u16 resp_opcode,
                    const char* resp_fmt, va_list ap);

/*
 * Same post-commit wait as ipc_rpc_call_va, but never returns -EINTR even
 * before send (kernel-internal completion RPCs, e.g. TASK_REAP_SYNC / VFS).
 */
i64 ipc_rpc_call_va_uninterruptible(Message_Port_t* server_port,
                                    Message_Port_t* reply_port, u16 req_opcode,
                                    const char* req_fmt, u16 resp_opcode,
                                    const char* resp_fmt, va_list ap);

/* Convenience: response opcode 0, format "q". */
i64 ipc_rpc_call(Message_Port_t* server_port, Message_Port_t* reply_port,
                 u16 req_opcode, const char* req_fmt, ...);

i64 ipc_rpc_call_named_va(const char* server_port_name,
                          Message_Port_t* reply_port, u16 req_opcode,
                          const char* req_fmt, u16 resp_opcode,
                          const char* resp_fmt, va_list ap);

i64 ipc_rpc_call_named(const char* server_port_name, Message_Port_t* reply_port,
                       u16 req_opcode, const char* req_fmt, ...);

i64 ipc_rpc_call_named_uninterruptible(const char* server_port_name,
                                       Message_Port_t* reply_port,
                                       u16 req_opcode, const char* req_fmt,
                                       ...);

/* Blocking rendezvous reply (default for all live request–reply servers). */
bool ipc_rpc_send_reply(u16 module, u16 resp_opcode, const char* resp_fmt,
                        const char* reply_port_name, i64 result);

void ipc_rpc_reply(const kmsg_t* km, const char* reply_port_name, u16 module,
                   u16 resp_opcode, const char* resp_fmt, i64 result);

/*
 * Handler for request–reply servers. Decode request, set *reply_port_out from
 * TLV 't', return i64 result for response (may be negative LINUX errno).
 */
typedef i64 (*ipc_rpc_server_handler_t)(u16 opcode, const kmsg_t* req,
                                        char** reply_port_out);

typedef void (*ipc_server_message_fn_t)(Message_t* msg, u16 service_id);

/*
 * Advance parked work (e.g. finished one-shot workers). Return value is
 * ignored by ipc_server_coop_loop (listen always blocks in recv_msg when
 * the port is empty — no schedule-spin on "still pending").
 */
typedef bool (*ipc_server_poll_fn_t)(void* ctx);

/*
 * Cooperative one-way listen (clean_server). Not a worker pool.
 */
void ipc_server_coop_loop(const char* listen_port_name,
                          ipc_server_message_fn_t on_message,
                          ipc_server_poll_fn_t poll_pending, void* poll_ctx);

/*
 * Transitional request–reply listen: blocking recv → handler → blocking
 * reply on the same thread. Does not spawn workers. Used by VFS/backends
 * until reply/nested IPC can be parked into ipc_server_coop_loop.
 */
void ipc_rpc_server_loop(const char* listen_port_name, u16 service_id,
                         u16 resp_opcode, const char* resp_fmt,
                         ipc_rpc_server_handler_t handler);

#endif /* _LINUX_COMPAT_IPC_RPC_H_ */
