#ifndef _LINUX_COMPAT_IPC_RPC_H_
#define _LINUX_COMPAT_IPC_RPC_H_

#include <common/stdarg.h>
#include <common/types.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/task/tcb.h>

/*
 * Shared request–reply IPC helpers for linux_layer servers (VFS, future RPC
 * services). Built on core send_msg/recv_msg + kmsg TLV (reply port = 't').
 *
 * One-way servers (e.g. clean_server) use ipc_server_recv_loop() only.
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

/*
 * Blocking RPC: variadic args match @req_fmt; reply port TLV 't' appended.
 * Response uses @resp_opcode + @resp_fmt (VFS passes KMSG_OP_VFS_RESP / "q").
 */
i64 ipc_rpc_call_va(Message_Port_t* server_port, Message_Port_t* reply_port,
                    u16 req_opcode, const char* req_fmt, u16 resp_opcode,
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

bool ipc_rpc_send_reply(u16 module, u16 resp_opcode, const char* resp_fmt,
                        const char* reply_port_name, i64 result);

void ipc_rpc_reply_best_effort(const kmsg_t* km, const char* reply_port_name,
                               u16 module, u16 resp_opcode,
                               const char* resp_fmt, i64 result);

/*
 * Handler for request–reply servers. Decode request, set *reply_port_out from
 * TLV 't', return i64 result for response (may be negative LINUX errno).
 */
typedef i64 (*ipc_rpc_server_handler_t)(u16 opcode, const kmsg_t* req,
                                        char** reply_port_out);

/* Block forever: recv on listen port, dispatch, always best-effort reply. */
void ipc_rpc_server_loop(const char* listen_port_name, u16 service_id,
                         u16 resp_opcode, const char* resp_fmt,
                         ipc_rpc_server_handler_t handler);

/*
 * One-way server loop (clean_server pattern): recv and invoke callback per
 * message; no automatic reply.
 */
typedef void (*ipc_server_message_fn_t)(Message_t* msg, u16 service_id);

void ipc_server_recv_loop(const char* listen_port_name,
                          ipc_server_message_fn_t on_message);

#endif /* _LINUX_COMPAT_IPC_RPC_H_ */
