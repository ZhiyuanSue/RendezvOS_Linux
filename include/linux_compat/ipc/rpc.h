#ifndef _LINUX_COMPAT_IPC_RPC_H_
#define _LINUX_COMPAT_IPC_RPC_H_

#include <common/dsa/list.h>
#include <common/stdarg.h>
#include <common/stdbool.h>
#include <common/types.h>
#include <linux_compat/ipc/port_naming.h>
#include <rendezvos/error.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/task/tcb.h>

/*
 * IPC helpers on core send_msg/recv_msg + kmsg TLV (reply port = 't').
 *
 * Listen models (no per-message OS worker pool — do not reintroduce):
 *   ipc_server_coop_loop       — one-way: try_recv + poll parked work
 *   ipc_rpc_coop_server_loop   — request–reply coop (park reply / nested)
 *   ipc_rpc_server_loop        — transitional blocking recv→handler→reply
 *                                (VFS/backends until they migrate)
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
 * Advance parked work between accepts (EXIT_NOTIFY try_send, etc.).
 * Return true if parked work remains that may need a yield (peer progress)
 * before the next recv — coop_loop then schedule() instead of blocking
 * recv_msg. Must not busy-spin without schedule.
 */
typedef bool (*ipc_server_poll_fn_t)(void* ctx);

/*
 * Cooperative one-way listen (clean_server reference). Not a worker pool.
 */
void ipc_server_coop_loop(const char* listen_port_name,
                          ipc_server_message_fn_t on_message,
                          ipc_server_poll_fn_t poll_pending, void* poll_ctx);

/*
 * Transitional request–reply listen: blocking recv → handler → blocking
 * reply on the same thread. Does not spawn workers. Used by VFS/backends
 * until they migrate to ipc_rpc_coop_server_loop.
 */
void ipc_rpc_server_loop(const char* listen_port_name, u16 service_id,
                         u16 resp_opcode, const char* resp_fmt,
                         ipc_rpc_server_handler_t handler);

/* ========================================================================
 * Request–reply cooperative server (park reply / nested RPC)
 *
 * Listen thread send_msg_queue is a single FIFO not demuxed by destination
 * port: at most one in-flight try_send payload may live there. The coop
 * queue serializes that slot (send_owner). Jobs waiting for reply keep only
 * (reply_port, result) until they own the slot.
 *
 * Nested: try_send to backend then try_recv on a dedicated reply port; while
 * in NESTED_RECV the send slot is free for client replies.
 * ======================================================================== */

typedef enum {
        IPC_RPC_COOP_ST_NONE = 0,
        IPC_RPC_COOP_ST_NEED_REPLY, /* result ready; need try_send to client */
        IPC_RPC_COOP_ST_NESTED_SEND, /* nested req on send queue; try_send */
        IPC_RPC_COOP_ST_NESTED_RECV, /* waiting try_recv on nested reply */
} ipc_rpc_coop_job_state_t;

typedef enum {
        /* Framework will reply with *result_out (or job already NEED_REPLY). */
        IPC_RPC_COOP_HANDLED = 0,
        /* Job stays parked (nested in flight / cookie work); no auto-reply. */
        IPC_RPC_COOP_PARKED = 1,
} ipc_rpc_coop_disp_t;

struct ipc_rpc_coop_queue;
struct ipc_rpc_coop_job;

typedef struct ipc_rpc_coop_job {
        struct list_entry node;
        struct ipc_rpc_coop_queue* q;
        ipc_rpc_coop_job_state_t state;
        char reply_port[PORT_NAME_LEN_MAX];
        i64 result;
        void* cookie; /* server-private */
        /* Nested RPC */
        char nested_server[PORT_NAME_LEN_MAX];
        Message_Port_t* nested_reply; /* held while nested active */
        u16 nested_resp_opcode;
        char nested_resp_fmt[8];
        i64 nested_result;
        bool reply_payload_queued; /* listen send queue holds our msg */
} ipc_rpc_coop_job_t;

typedef struct ipc_rpc_coop_queue {
        struct list_entry jobs;
        bool inited;
        ipc_rpc_coop_job_t* send_owner; /* exclusive listen send-queue user */
        void (*resume_fn)(ipc_rpc_coop_job_t* job, i64 nested_result,
                          void* ctx);
        void* resume_ctx;
} ipc_rpc_coop_queue_t;

/*
 * Coop handler: job->reply_port already set from request TLV 't'.
 * HANDLED: set *result_out (unless already NEED_REPLY via set_result).
 * PARKED: nested_call / cookie; resume_fn or later set_result completes.
 */
typedef ipc_rpc_coop_disp_t (*ipc_rpc_coop_handler_t)(ipc_rpc_coop_job_t* job,
                                                      u16 opcode,
                                                      const kmsg_t* km,
                                                      i64* result_out);

void ipc_rpc_coop_queue_init(ipc_rpc_coop_queue_t* q);

void ipc_rpc_coop_queue_set_resume(ipc_rpc_coop_queue_t* q,
                                   void (*resume_fn)(ipc_rpc_coop_job_t* job,
                                                     i64 nested_result,
                                                     void* ctx),
                                   void* resume_ctx);

/* Alloc+link job; copies reply_port. NULL on OOM / bad name. */
ipc_rpc_coop_job_t* ipc_rpc_coop_job_create(ipc_rpc_coop_queue_t* q,
                                            const char* reply_port);

/* Mark NEED_REPLY (clears nested state except held ports released). */
void ipc_rpc_coop_job_set_result(ipc_rpc_coop_job_t* job, i64 result);

/* Unlink, drop send ownership / nested port, free. */
void ipc_rpc_coop_job_release(ipc_rpc_coop_job_t* job);

/*
 * Begin nested RPC from the listen thread.
 * SUCCESS → job NESTED_RECV (send slot free).
 * -E_REND_AGAIN → NESTED_SEND if we own the send slot; or slot busy (no state
 * change — caller should PARKED and retry later).
 * Other errors: hard fail (job unchanged or released by caller).
 */
error_t ipc_rpc_coop_nested_call_va(ipc_rpc_coop_job_t* job,
                                    const char* server_port_name,
                                    Message_Port_t* nested_reply,
                                    u16 req_opcode, const char* req_fmt,
                                    u16 resp_opcode, const char* resp_fmt,
                                    va_list ap);

error_t ipc_rpc_coop_nested_call(ipc_rpc_coop_job_t* job,
                                 const char* server_port_name,
                                 Message_Port_t* nested_reply, u16 req_opcode,
                                 const char* req_fmt, u16 resp_opcode,
                                 const char* resp_fmt, ...);

/*
 * Progress NEED_REPLY / NESTED_* jobs. Never schedule()-spin.
 * module/resp_* are the *client* reply kmsg (VFS: service_id + VFS_RESP).
 */
void ipc_rpc_coop_queue_poll(ipc_rpc_coop_queue_t* q, u16 module,
                             u16 resp_opcode, const char* resp_fmt);

/*
 * Request–reply coop listen. poll_extra runs after queue_poll each turn
 * (e.g. server-private FSM). Does not spawn workers.
 */
void ipc_rpc_coop_server_loop(const char* listen_port_name, u16 service_id,
                              u16 resp_opcode, const char* resp_fmt,
                              ipc_rpc_coop_handler_t handler,
                              ipc_rpc_coop_queue_t* q,
                              ipc_server_poll_fn_t poll_extra,
                              void* poll_extra_ctx);

#endif /* _LINUX_COMPAT_IPC_RPC_H_ */
