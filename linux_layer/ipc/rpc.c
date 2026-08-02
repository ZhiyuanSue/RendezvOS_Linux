/*
 * linux_compat IPC RPC framework (request–reply + one-way server loops).
 */

#include <common/atomic.h>
#include <common/dsa/list.h>
#include <common/dsa/ms_queue.h>
#include <common/refcount.h>
#include <common/stddef.h>
#include <common/string.h>
#include <common/taggedptr.h>
#include <linux_compat/errno.h>
#include <linux_compat/ipc/block_wake.h>
#include <linux_compat/ipc/port_naming.h>
#include <linux_compat/ipc/rpc.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/signal/signal_deliver.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/ipc_serial.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>

extern struct Port_Table* global_port_table;

static size_t ipc_port_name_copy_service(char* buf, size_t bufsize,
                                         const char* service)
{
        size_t n = 0;

        if (!buf || bufsize == 0 || !service || !service[0]) {
                if (buf && bufsize)
                        buf[0] = '\0';
                return 0;
        }
        while (service[n] && n + 1 < bufsize) {
                buf[n] = service[n];
                n++;
        }
        if (service[n]) {
                buf[0] = '\0';
                return 0;
        }
        buf[n] = '\0';
        return n;
}

size_t ipc_port_name_cli(char* buf, size_t bufsize, const char* service,
                         pid_t caller_id)
{
        size_t n = ipc_port_name_copy_service(buf, bufsize, service);
        const char* mid = "_cli_";
        size_t i;

        if (!n)
                return 0;
        for (i = 0; mid[i]; i++) {
                if (n + 1 >= bufsize) {
                        buf[0] = '\0';
                        return 0;
                }
                buf[n++] = mid[i];
        }
        buf[n] = '\0';
        if (proc_format_pid(buf + n, bufsize - n, caller_id) == 0) {
                buf[0] = '\0';
                return 0;
        }
        return strlen(buf);
}

size_t ipc_port_name_listen_cpu(char* buf, size_t bufsize, const char* service,
                                cpu_id_t cpu)
{
        size_t n = ipc_port_name_copy_service(buf, bufsize, service);
        const char* mid = "_c";
        size_t i;

        if (!n)
                return 0;
        for (i = 0; mid[i]; i++) {
                if (n + 1 >= bufsize) {
                        buf[0] = '\0';
                        return 0;
                }
                buf[n++] = mid[i];
        }
        buf[n] = '\0';
        /* Decimal cpu id; same digit helper as pid strings. */
        if (proc_format_pid(buf + n, bufsize - n, (pid_t)cpu) == 0) {
                buf[0] = '\0';
                return 0;
        }
        return strlen(buf);
}

const char* ipc_serial_payload_reply_port(const u8* payload, u32 len)
{
        u32 nparam;
        u32 off;
        u32 i;
        const char* last = NULL;

        if (!payload || len < 4u) {
                return NULL;
        }

        memcpy(&nparam, payload, sizeof(nparam));
        off = 4u;
        for (i = 0; i < nparam; i++) {
                u8 tag;
                u32 vlen;

                if (off + 1u + 4u > len) {
                        return last;
                }
                tag = payload[off++];
                memcpy(&vlen, payload + off, sizeof(vlen));
                off += 4u;
                if (off + vlen > len) {
                        return last;
                }
                if (tag == (u8)'t' && vlen > 0u) {
                        last = (const char*)(payload + off);
                }
                off += vlen;
        }
        return last;
}

void ipc_rpc_drain_recv_queue(void)
{
        Thread_Base* self = get_cpu_current_thread();
        Message_t* stale;

        if (self
            && atomic64_load(
                       (volatile const u64*)&self->recv_pending_cnt.counter)
                       == 0) {
                return;
        }

        while ((stale = dequeue_recv_msg()) != NULL) {
                ref_put(&stale->ms_queue_node.refcount, free_message_ref);
        }
}

size_t ipc_rpc_format_port_name(char* buf, size_t bufsize, const char* prefix,
                                pid_t pid)
{
        size_t i;
        size_t plen;

        if (!buf || bufsize == 0 || !prefix) {
                return 0;
        }

        plen = strlen(prefix);
        if (plen >= bufsize) {
                buf[0] = '\0';
                return 0;
        }

        memcpy(buf, prefix, plen);
        i = plen;

        if (proc_format_pid(buf + i, bufsize - i, pid) == 0) {
                buf[0] = '\0';
                return 0;
        }

        return strlen(buf);
}

Message_Port_t* ipc_rpc_port_lookup_or_create(const char* port_name)
{
        Message_Port_t* port;

        if (!port_name || !port_name[0] || !global_port_table) {
                return NULL;
        }

        port = thread_lookup_port(port_name);
        if (port) {
                return port;
        }

        port = create_message_port(port_name);
        if (!port) {
                return NULL;
        }

        if (register_port(global_port_table, port) != REND_SUCCESS) {
                delete_message_port_structure(port);
                return NULL;
        }

        return port;
}

void ipc_rpc_unregister_port_by_pid(const char* prefix, pid_t pid)
{
        char port_name[PORT_NAME_LEN_MAX];

        if (!global_port_table || !prefix || pid <= 0) {
                return;
        }
        if (ipc_rpc_format_port_name(port_name, sizeof(port_name), prefix, pid)
            == 0) {
                return;
        }
        (void)unregister_port(global_port_table, port_name);
}

void ipc_rpc_unregister_port_name(const char* port_name)
{
        if (!global_port_table || !port_name || !port_name[0]) {
                return;
        }
        (void)unregister_port(global_port_table, port_name);
}

static bool ipc_rpc_recv_is_interrupt(Message_Port_t* reply_port,
                                      Message_t* msg)
{
        const kmsg_t* km;

        if (!reply_port || !msg) {
                return false;
        }

        km = kmsg_from_msg(msg);
        if (!km || km->hdr.module != reply_port->service_id) {
                return false;
        }
        return km->hdr.opcode == KMSG_OP_IPC_RECV_INTERRUPT;
}

static error_t ipc_payload_append_reply_port(u8* payload, u32 cap,
                                             u32* inout_len,
                                             const char* reply_port)
{
        u32 off;
        u32 slen;
        u32* nparam;

        if (!payload || !inout_len || !reply_port) {
                return -E_IN_PARAM;
        }

        off = *inout_len;
        slen = (u32)strlen(reply_port) + 1u;
        if (off + 1u + 4u + slen > cap) {
                return -E_IN_PARAM;
        }

        nparam = (u32*)payload;
        (*nparam)++;

        payload[off++] = (u8)'t';
        memcpy(payload + off, &slen, sizeof(slen));
        off += 4u;
        memcpy(payload + off, reply_port, slen);
        off += slen;

        *inout_len = off;
        return REND_SUCCESS;
}

static Msg_Data_t* ipc_kmsg_create_request(u16 module, u16 opcode,
                                           const char* fmt,
                                           const char* reply_port, va_list ap)
{
        struct allocator* alloc = percpu(kallocator);
        u32 len_fmt;
        u32 payload_len;
        va_list ap_copy;
        kmsg_t* km;
        const size_t km_hdr_sz = offsetof(kmsg_t, payload);
        size_t km_size;
        u32 t_tlv;

        if (!alloc || !fmt || !reply_port) {
                return NULL;
        }

        va_copy(ap_copy, ap);
        if (ipc_serial_measure_va(fmt, ap_copy, &len_fmt) != REND_SUCCESS) {
                va_end(ap_copy);
                return NULL;
        }
        va_end(ap_copy);

        t_tlv = 1u + 4u + (u32)strlen(reply_port) + 1u;
        if (len_fmt > KMSG_MAX_PAYLOAD - t_tlv) {
                return NULL;
        }
        payload_len = len_fmt + t_tlv;

        if (payload_len > KMSG_MAX_PAYLOAD
            || payload_len > (u32)(SIZE_MAX - km_hdr_sz)) {
                return NULL;
        }

        km_size = km_hdr_sz + (size_t)payload_len;
        km = (kmsg_t*)alloc->m_alloc(alloc, km_size);
        if (!km) {
                return NULL;
        }
        memset(km, 0, km_size);

        km->hdr.magic = KMSG_MAGIC;
        km->hdr.module = module;
        km->hdr.opcode = opcode;

        va_copy(ap_copy, ap);
        if (ipc_serial_encode_into_va(km->payload, len_fmt, fmt, ap_copy)
            != REND_SUCCESS) {
                va_end(ap_copy);
                alloc->m_free(alloc, km);
                return NULL;
        }
        va_end(ap_copy);

        payload_len = len_fmt;
        if (ipc_payload_append_reply_port(
                    km->payload, len_fmt + t_tlv, &payload_len, reply_port)
            != REND_SUCCESS) {
                alloc->m_free(alloc, km);
                return NULL;
        }
        km->hdr.payload_len = payload_len;

        {
                void* data = (void*)km;
                return create_message_data(MSG_DATA_TAG_KMSG,
                                           (u64)km_size,
                                           &data,
                                           free_msgdata_ref_default);
        }
}

static i64 ipc_rpc_call_va_flags(Message_Port_t* server_port,
                                 Message_Port_t* reply_port, u16 req_opcode,
                                 const char* req_fmt, u16 resp_opcode,
                                 const char* resp_fmt, bool interruptible,
                                 va_list ap)
{
        Msg_Data_t* msg_data;
        Message_t* msg;
        error_t err;
        i64 result = 0;
        u16 module;
        const char* rfmt = resp_fmt ? resp_fmt : IPC_RPC_RESP_FMT_DEFAULT;

        if (!server_port || !reply_port) {
                return -LINUX_EINVAL;
        }

        /*
         * Interruptible only before the request is committed. After send_msg
         * succeeds, the server (often single-threaded) may already hold work
         * and will block in send_msg(reply); abandoning recv wedges listen.
         */
        if (interruptible && linux_signal_has_deliverable_pending()) {
                return -LINUX_EINTR;
        }

        module = server_port->service_id;
        ipc_rpc_drain_recv_queue();

        msg_data = ipc_kmsg_create_request(
                module, req_opcode, req_fmt, reply_port->name, ap);
        if (!msg_data) {
                return -LINUX_EINVAL;
        }

        msg = create_message_with_msg(msg_data);
        /* Drop creator ref; message holds its own (see
         * create_message_with_msg). */
        ref_put(&msg_data->refcount, free_msgdata_ref_default);
        if (!msg) {
                return -LINUX_ENOMEM;
        }

        err = enqueue_msg_for_send(msg);
        if (err != REND_SUCCESS) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                return -LINUX_EIO;
        }

        err = send_msg(server_port);
        if (err != REND_SUCCESS) {
                /* Includes -E_REND_PORT_CLOSED if listen/reply port was closed.
                 */
                return -LINUX_EIO;
        }

        /*
         * Committed: reply is owed. Stay in blocking recv_msg until a reply
         * or port close. Do not bare schedule() on errors (no port wait
         * identity → EBR spin). Re-enter recv_msg for non-fatal returns.
         */
        for (;;) {
                err = recv_msg(reply_port);
                if (err == -E_REND_PORT_CLOSED) {
                        ipc_rpc_drain_recv_queue();
                        return -LINUX_EIO;
                }
                if (err != REND_SUCCESS) {
                        /*
                         * Transfer/rendezvous hiccup: core may surface an
                         * error after waking us; reply can still arrive.
                         * Block again in recv_msg — never yield-spin.
                         */
                        continue;
                }

                msg = dequeue_recv_msg();
                if (!msg) {
                        continue;
                }

                if (ipc_rpc_recv_is_interrupt(reply_port, msg)) {
                        /* Signal wake only; reply still owed after commit. */
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                        continue;
                }

                if (linux_ipc_kmsg_is_port_closed(reply_port, msg)) {
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                        return -LINUX_EIO;
                }

                {
                        const kmsg_t* resp_kmsg = kmsg_from_msg(msg);
                        if (!resp_kmsg || resp_kmsg->hdr.module != module
                            || resp_kmsg->hdr.opcode != resp_opcode) {
                                /* Stale/unexpected; keep waiting for reply. */
                                ref_put(&msg->ms_queue_node.refcount,
                                        free_message_ref);
                                continue;
                        }

                        err = ipc_serial_decode(resp_kmsg->payload,
                                                resp_kmsg->hdr.payload_len,
                                                rfmt,
                                                &result);
                        if (err != REND_SUCCESS) {
                                ref_put(&msg->ms_queue_node.refcount,
                                        free_message_ref);
                                return -LINUX_EIO;
                        }
                }

                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                return result;
        }
}

i64 ipc_rpc_call_va(Message_Port_t* server_port, Message_Port_t* reply_port,
                    u16 req_opcode, const char* req_fmt, u16 resp_opcode,
                    const char* resp_fmt, va_list ap)
{
        return ipc_rpc_call_va_flags(server_port,
                                     reply_port,
                                     req_opcode,
                                     req_fmt,
                                     resp_opcode,
                                     resp_fmt,
                                     true,
                                     ap);
}

i64 ipc_rpc_call_va_uninterruptible(Message_Port_t* server_port,
                                    Message_Port_t* reply_port, u16 req_opcode,
                                    const char* req_fmt, u16 resp_opcode,
                                    const char* resp_fmt, va_list ap)
{
        return ipc_rpc_call_va_flags(server_port,
                                     reply_port,
                                     req_opcode,
                                     req_fmt,
                                     resp_opcode,
                                     resp_fmt,
                                     false,
                                     ap);
}

i64 ipc_rpc_call(Message_Port_t* server_port, Message_Port_t* reply_port,
                 u16 req_opcode, const char* req_fmt, ...)
{
        va_list ap;
        i64 ret;

        va_start(ap, req_fmt);
        ret = ipc_rpc_call_va(server_port,
                              reply_port,
                              req_opcode,
                              req_fmt,
                              IPC_RPC_RESP_OPCODE_DEFAULT,
                              IPC_RPC_RESP_FMT_DEFAULT,
                              ap);
        va_end(ap);
        return ret;
}

i64 ipc_rpc_call_named_va(const char* server_port_name,
                          Message_Port_t* reply_port, u16 req_opcode,
                          const char* req_fmt, u16 resp_opcode,
                          const char* resp_fmt, va_list ap)
{
        Message_Port_t* server_port;
        i64 ret;

        if (!server_port_name || !reply_port) {
                return -LINUX_EINVAL;
        }

        server_port = thread_lookup_port(server_port_name);
        if (!server_port) {
                return -LINUX_ENOSYS;
        }

        ret = ipc_rpc_call_va(server_port,
                              reply_port,
                              req_opcode,
                              req_fmt,
                              resp_opcode,
                              resp_fmt,
                              ap);
        ref_put(&server_port->refcount, free_message_port_ref);
        return ret;
}

i64 ipc_rpc_call_named(const char* server_port_name, Message_Port_t* reply_port,
                       u16 req_opcode, const char* req_fmt, ...)
{
        va_list ap;
        i64 ret;

        va_start(ap, req_fmt);
        ret = ipc_rpc_call_named_va(server_port_name,
                                    reply_port,
                                    req_opcode,
                                    req_fmt,
                                    IPC_RPC_RESP_OPCODE_DEFAULT,
                                    IPC_RPC_RESP_FMT_DEFAULT,
                                    ap);
        va_end(ap);
        return ret;
}

i64 ipc_rpc_call_named_uninterruptible(const char* server_port_name,
                                       Message_Port_t* reply_port,
                                       u16 req_opcode, const char* req_fmt, ...)
{
        Message_Port_t* server_port;
        va_list ap;
        i64 ret;

        if (!server_port_name || !reply_port) {
                return -LINUX_EINVAL;
        }

        server_port = thread_lookup_port(server_port_name);
        if (!server_port) {
                return -LINUX_ENOSYS;
        }

        va_start(ap, req_fmt);
        ret = ipc_rpc_call_va_uninterruptible(server_port,
                                              reply_port,
                                              req_opcode,
                                              req_fmt,
                                              IPC_RPC_RESP_OPCODE_DEFAULT,
                                              IPC_RPC_RESP_FMT_DEFAULT,
                                              ap);
        va_end(ap);
        ref_put(&server_port->refcount, free_message_port_ref);
        return ret;
}

/*
 * Blocking rendezvous reply. Live clients block in ipc_rpc_call* recv.
 * Abandoned clients: unregister → PORT_CLOSED; treat as handled.
 * Do not use try_send — races server-before-client-recv.
 *
 * Pre-send alloc/enqueue may yield+retry (client still waiting).
 * After enqueue+send_msg: SUCCESS / PORT_CLOSED are terminal.
 * -E_REND_NO_MSG / -E_REND_AGAIN: payload was not delivered — rebuild and
 * retry (not a duplicate). Other post-send errors: fail (listen continues).
 */
bool ipc_rpc_send_reply(u16 module, u16 resp_opcode, const char* resp_fmt,
                        const char* reply_port_name, i64 result)
{
        Message_Port_t* reply_port;
        Msg_Data_t* resp_data;
        Message_t* resp_msg;
        error_t send_err;
        const char* rfmt = resp_fmt ? resp_fmt : IPC_RPC_RESP_FMT_DEFAULT;

        if (!reply_port_name || !reply_port_name[0]) {
                return false;
        }

        for (;;) {
                reply_port = thread_lookup_port(reply_port_name);
                if (!reply_port) {
                        return true;
                }

                resp_data = kmsg_create(module, resp_opcode, rfmt, result);
                if (!resp_data) {
                        ref_put(&reply_port->refcount, free_message_port_ref);
                        schedule(percpu(core_tm));
                        continue;
                }

                resp_msg = create_message_with_msg(resp_data);
                ref_put(&resp_data->refcount, resp_data->free_data);
                if (!resp_msg) {
                        ref_put(&reply_port->refcount, free_message_port_ref);
                        schedule(percpu(core_tm));
                        continue;
                }

                send_err = enqueue_msg_for_send(resp_msg);
                if (send_err != REND_SUCCESS) {
                        ref_put(&resp_msg->ms_queue_node.refcount,
                                free_message_ref);
                        ref_put(&reply_port->refcount, free_message_port_ref);
                        schedule(percpu(core_tm));
                        continue;
                }

                /*
                 * Payload is on this thread's send slot — send_msg blocks
                 * until rendezvous. PORT_CLOSED: client tore down; core
                 * dropped the orphan — treat as handled.
                 */
                send_err = send_msg(reply_port);
                ref_put(&reply_port->refcount, free_message_port_ref);
                if (send_err == REND_SUCCESS
                    || send_err == -E_REND_PORT_CLOSED) {
                        return true;
                }
                /*
                 * NO_MSG: transfer abandoned / empty payload path — not
                 * delivered, safe to rebuild. AGAIN: rare surface from
                 * send_msg; same recovery (core usually loops internally).
                 */
                if (send_err == -E_REND_NO_MSG || send_err == -E_REND_AGAIN) {
                        schedule(percpu(core_tm));
                        continue;
                }
                pr_error("[IPC-RPC] reply send failed port='%s' err=%d "
                         "result=%ld (client may still be in recv)\n",
                         reply_port_name,
                         (int)send_err,
                         result);
                return false;
        }
}

void ipc_rpc_reply(const kmsg_t* km, const char* reply_port_name, u16 module,
                   u16 resp_opcode, const char* resp_fmt, i64 result)
{
        char reply_copy[PORT_NAME_LEN_MAX];
        const char* reply = reply_port_name;

        if ((!reply || !reply[0]) && km) {
                reply = ipc_serial_payload_reply_port(km->payload,
                                                      km->hdr.payload_len);
        }

        if (!reply || !reply[0]) {
                pr_error("[IPC-RPC] no reply port (result=%ld)\n", result);
                return;
        }

        strncpy(reply_copy, reply, sizeof(reply_copy) - 1u);
        reply_copy[sizeof(reply_copy) - 1u] = '\0';

        if (!ipc_rpc_send_reply(
                    module, resp_opcode, resp_fmt, reply_copy, result)) {
                /*
                 * Post-send failure only (pre-send retries until port gone).
                 * Still return to server_loop so listen is not wedged.
                 */
                pr_error("[IPC-RPC] reply incomplete port='%s' result=%ld\n",
                         reply_copy,
                         result);
        }
}

static Message_Port_t* ipc_server_coop_lookup(const char* listen_port_name)
{
        Message_Port_t* port = NULL;

        while (!port) {
                port = thread_lookup_port(listen_port_name);
                if (!port) {
                        schedule(percpu(core_tm));
                }
        }
        return port;
}

static void ipc_server_coop_drain(Message_Port_t** port_io,
                                  const char* listen_port_name,
                                  ipc_server_message_fn_t on_message)
{
        Message_Port_t* port = *port_io;

        while (1) {
                Message_t* msg = dequeue_recv_msg();

                if (!msg) {
                        break;
                }
                if (linux_ipc_kmsg_is_port_closed(port, msg)) {
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                        ref_put(&port->refcount, free_message_port_ref);
                        *port_io = ipc_server_coop_lookup(listen_port_name);
                        break;
                }
                on_message(msg, port->service_id);
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
        }
}

void ipc_server_coop_loop(const char* listen_port_name,
                          ipc_server_message_fn_t on_message,
                          ipc_server_poll_fn_t poll_pending, void* poll_ctx)
{
        Message_Port_t* port;

        if (!listen_port_name || !on_message) {
                return;
        }

        port = ipc_server_coop_lookup(listen_port_name);

        /*
         * Poll parked work, then try_recv. Block in recv_msg only when there
         * is no parked outbound work; otherwise schedule once so the peer
         * (e.g. parent wait4) can progress, then re-poll try_send.
         */
        while (1) {
                error_t ret;
                bool parked_pending = false;

                if (poll_pending)
                        parked_pending = poll_pending(poll_ctx);

                ret = ipc_try_recv_msg(port);
                if (ret == REND_SUCCESS) {
                        ipc_server_coop_drain(
                                &port, listen_port_name, on_message);
                        continue;
                }

                if (ret == -E_REND_PORT_CLOSED) {
                        ref_put(&port->refcount, free_message_port_ref);
                        port = ipc_server_coop_lookup(listen_port_name);
                        continue;
                }

                if (parked_pending) {
                        schedule(percpu(core_tm));
                        continue;
                }

                ret = recv_msg(port);
                if (ret != REND_SUCCESS) {
                        if (ret == -E_REND_PORT_CLOSED) {
                                ref_put(&port->refcount, free_message_port_ref);
                                port = ipc_server_coop_lookup(listen_port_name);
                        }
                        continue;
                }

                ipc_server_coop_drain(&port, listen_port_name, on_message);
        }
}

/*
 * Same-thread blocking request–reply. Not a worker pool: handler and
 * send_msg(reply) run on the listen thread (can wedge the whole service).
 */
void ipc_rpc_server_loop(const char* listen_port_name, u16 service_id,
                         u16 resp_opcode, const char* resp_fmt,
                         ipc_rpc_server_handler_t handler)
{
        Message_Port_t* port = NULL;

        if (!listen_port_name || !handler) {
                return;
        }

        while (!port) {
                port = thread_lookup_port(listen_port_name);
                if (!port) {
                        schedule(percpu(core_tm));
                }
        }

        pr_info("[IPC-RPC] server loop on '%s' service_id=%u\n",
                listen_port_name,
                service_id);

        while (1) {
                error_t ret = recv_msg(port);
                if (ret != REND_SUCCESS) {
                        ref_put(&port->refcount, free_message_port_ref);
                        port = NULL;
                        while (!port) {
                                port = thread_lookup_port(listen_port_name);
                                if (!port) {
                                        schedule(percpu(core_tm));
                                }
                        }
                        continue;
                }

                while (1) {
                        Message_t* msg = dequeue_recv_msg();
                        const kmsg_t* km;
                        char* reply_port = NULL;
                        i64 result;

                        if (!msg) {
                                break;
                        }

                        km = kmsg_from_msg(msg);
                        if (!km || km->hdr.module != service_id) {
                                ipc_rpc_reply(km,
                                              NULL,
                                              service_id,
                                              resp_opcode,
                                              resp_fmt,
                                              -LINUX_EIO);
                                ref_put(&msg->ms_queue_node.refcount,
                                        free_message_ref);
                                continue;
                        }

                        if (linux_ipc_kmsg_is_port_closed(port, msg)) {
                                ref_put(&msg->ms_queue_node.refcount,
                                        free_message_ref);
                                ref_put(&port->refcount, free_message_port_ref);
                                port = NULL;
                                while (!port) {
                                        port = thread_lookup_port(
                                                listen_port_name);
                                        if (!port) {
                                                schedule(percpu(core_tm));
                                        }
                                }
                                break;
                        }

                        result = handler(km->hdr.opcode, km, &reply_port);
                        ipc_rpc_reply(km,
                                      reply_port,
                                      service_id,
                                      resp_opcode,
                                      resp_fmt,
                                      result);
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                }
        }
}

/* ========================================================================
 * Request–reply cooperative server
 * ======================================================================== */

static void ipc_rpc_abandon_current_send_payload(void)
{
        Thread_Base* self = get_cpu_current_thread();
        Message_t* pending;
        tagged_ptr_t dq;

        if (!self)
                return;

        pending = (Message_t*)atomic64_exchange(
                (volatile u64*)&self->send_pending_msg, (u64)NULL);
        if (pending) {
                ref_put(&pending->ms_queue_node.refcount, free_message_ref);
                return;
        }

        dq = msq_dequeue(&self->send_msg_queue, free_message_ref);
        if (!tp_is_none(dq)) {
                Message_t* msg =
                        container_of(tp_get_ptr(dq), Message_t, ms_queue_node);
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
        }
}

static void ipc_rpc_coop_clear_send_owner(ipc_rpc_coop_queue_t* q,
                                          ipc_rpc_coop_job_t* job)
{
        if (!q || !job)
                return;
        if (q->send_owner == job) {
                if (job->reply_payload_queued) {
                        ipc_rpc_abandon_current_send_payload();
                        job->reply_payload_queued = false;
                }
                q->send_owner = NULL;
        }
}

static void ipc_rpc_coop_nested_release_port(ipc_rpc_coop_job_t* job)
{
        if (!job || !job->nested_reply)
                return;
        ref_put(&job->nested_reply->refcount, free_message_port_ref);
        job->nested_reply = NULL;
}

void ipc_rpc_coop_queue_init(ipc_rpc_coop_queue_t* q)
{
        if (!q)
                return;
        INIT_LIST_HEAD(&q->jobs);
        q->inited = true;
        q->send_owner = NULL;
        q->resume_fn = NULL;
        q->resume_ctx = NULL;
}

void ipc_rpc_coop_queue_set_resume(ipc_rpc_coop_queue_t* q,
                                   void (*resume_fn)(ipc_rpc_coop_job_t* job,
                                                     i64 nested_result,
                                                     void* ctx),
                                   void* resume_ctx)
{
        if (!q)
                return;
        q->resume_fn = resume_fn;
        q->resume_ctx = resume_ctx;
}

ipc_rpc_coop_job_t* ipc_rpc_coop_job_create(ipc_rpc_coop_queue_t* q,
                                            const char* reply_port)
{
        struct allocator* alloc = percpu(kallocator);
        ipc_rpc_coop_job_t* job;

        if (!q || !q->inited || !reply_port || !reply_port[0] || !alloc
            || !alloc->m_alloc)
                return NULL;

        job = (ipc_rpc_coop_job_t*)alloc->m_alloc(alloc, sizeof(*job));
        if (!job)
                return NULL;
        memset(job, 0, sizeof(*job));
        INIT_LIST_HEAD(&job->node);
        job->q = q;
        job->state = IPC_RPC_COOP_ST_NONE;
        strncpy(job->reply_port, reply_port, sizeof(job->reply_port) - 1u);
        job->reply_port[sizeof(job->reply_port) - 1u] = '\0';
        list_add_tail(&job->node, &q->jobs);
        return job;
}

void ipc_rpc_coop_job_set_result(ipc_rpc_coop_job_t* job, i64 result)
{
        if (!job)
                return;
        ipc_rpc_coop_clear_send_owner(job->q, job);
        ipc_rpc_coop_nested_release_port(job);
        job->nested_server[0] = '\0';
        job->result = result;
        job->state = IPC_RPC_COOP_ST_NEED_REPLY;
}

void ipc_rpc_coop_job_release(ipc_rpc_coop_job_t* job)
{
        struct allocator* alloc = percpu(kallocator);

        if (!job)
                return;
        ipc_rpc_coop_clear_send_owner(job->q, job);
        ipc_rpc_coop_nested_release_port(job);
        list_del_init(&job->node);
        if (alloc && alloc->m_free)
                alloc->m_free(alloc, job);
}

static error_t ipc_rpc_coop_enqueue_kmsg(Msg_Data_t* md)
{
        Message_t* msg;
        error_t e;

        if (!md)
                return -E_RENDEZVOS;
        msg = create_message_with_msg(md);
        ref_put(&md->refcount, md->free_data);
        if (!msg)
                return -E_RENDEZVOS;
        e = enqueue_msg_for_send(msg);
        if (e != REND_SUCCESS) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                return e;
        }
        return REND_SUCCESS;
}

error_t ipc_rpc_coop_nested_call_va(ipc_rpc_coop_job_t* job,
                                    const char* server_port_name,
                                    Message_Port_t* nested_reply,
                                    u16 req_opcode, const char* req_fmt,
                                    u16 resp_opcode, const char* resp_fmt,
                                    va_list ap)
{
        Message_Port_t* server;
        Msg_Data_t* md;
        error_t e;
        const char* rfmt = resp_fmt ? resp_fmt : IPC_RPC_RESP_FMT_DEFAULT;

        if (!job || !job->q || !server_port_name || !nested_reply || !req_fmt)
                return -E_IN_PARAM;

        if (job->q->send_owner && job->q->send_owner != job)
                return -E_REND_AGAIN;

        if (job->state == IPC_RPC_COOP_ST_NESTED_SEND
            && job->reply_payload_queued && job->q->send_owner == job) {
                server = thread_lookup_port(job->nested_server);
                if (!server)
                        return -E_RENDEZVOS;
                e = ipc_try_send_msg(server);
                ref_put(&server->refcount, free_message_port_ref);
                if (e == REND_SUCCESS) {
                        job->reply_payload_queued = false;
                        job->q->send_owner = NULL;
                        job->state = IPC_RPC_COOP_ST_NESTED_RECV;
                        return REND_SUCCESS;
                }
                if (e == -E_REND_AGAIN)
                        return -E_REND_AGAIN;
                ipc_rpc_coop_clear_send_owner(job->q, job);
                return e;
        }

        if (job->state != IPC_RPC_COOP_ST_NONE)
                return -E_REND_AGAIN;

        if (!ref_get_not_zero(&nested_reply->refcount))
                return -E_RENDEZVOS;

        server = thread_lookup_port(server_port_name);
        if (!server) {
                ref_put(&nested_reply->refcount, free_message_port_ref);
                return -E_RENDEZVOS;
        }

        md = ipc_kmsg_create_request(server->service_id,
                                     req_opcode,
                                     req_fmt,
                                     nested_reply->name,
                                     ap);
        if (!md) {
                ref_put(&server->refcount, free_message_port_ref);
                ref_put(&nested_reply->refcount, free_message_port_ref);
                return -E_RENDEZVOS;
        }

        e = ipc_rpc_coop_enqueue_kmsg(md);
        if (e != REND_SUCCESS) {
                ref_put(&server->refcount, free_message_port_ref);
                ref_put(&nested_reply->refcount, free_message_port_ref);
                return e;
        }

        ipc_rpc_coop_nested_release_port(job);
        job->nested_reply = nested_reply;
        strncpy(job->nested_server,
                server_port_name,
                sizeof(job->nested_server) - 1u);
        job->nested_server[sizeof(job->nested_server) - 1u] = '\0';
        job->nested_resp_opcode = resp_opcode;
        strncpy(job->nested_resp_fmt, rfmt, sizeof(job->nested_resp_fmt) - 1u);
        job->nested_resp_fmt[sizeof(job->nested_resp_fmt) - 1u] = '\0';
        job->reply_payload_queued = true;
        job->q->send_owner = job;
        job->state = IPC_RPC_COOP_ST_NESTED_SEND;

        e = ipc_try_send_msg(server);
        ref_put(&server->refcount, free_message_port_ref);
        if (e == REND_SUCCESS) {
                job->reply_payload_queued = false;
                job->q->send_owner = NULL;
                job->state = IPC_RPC_COOP_ST_NESTED_RECV;
                return REND_SUCCESS;
        }
        if (e == -E_REND_AGAIN)
                return -E_REND_AGAIN;

        ipc_rpc_coop_clear_send_owner(job->q, job);
        ipc_rpc_coop_nested_release_port(job);
        job->state = IPC_RPC_COOP_ST_NONE;
        return e;
}

error_t ipc_rpc_coop_nested_call(ipc_rpc_coop_job_t* job,
                                 const char* server_port_name,
                                 Message_Port_t* nested_reply, u16 req_opcode,
                                 const char* req_fmt, u16 resp_opcode,
                                 const char* resp_fmt, ...)
{
        va_list ap;
        error_t e;

        va_start(ap, resp_fmt);
        e = ipc_rpc_coop_nested_call_va(job,
                                        server_port_name,
                                        nested_reply,
                                        req_opcode,
                                        req_fmt,
                                        resp_opcode,
                                        resp_fmt,
                                        ap);
        va_end(ap);
        return e;
}

static error_t ipc_rpc_coop_try_client_reply(ipc_rpc_coop_job_t* job,
                                             u16 module, u16 resp_opcode,
                                             const char* resp_fmt)
{
        Message_Port_t* reply_port;
        Msg_Data_t* md;
        error_t e;
        const char* rfmt = resp_fmt ? resp_fmt : IPC_RPC_RESP_FMT_DEFAULT;

        if (!job || job->state != IPC_RPC_COOP_ST_NEED_REPLY || !job->q)
                return -E_IN_PARAM;

        if (job->q->send_owner && job->q->send_owner != job)
                return -E_REND_AGAIN;

        if (!job->reply_payload_queued) {
                reply_port = thread_lookup_port(job->reply_port);
                if (!reply_port) {
                        /* Client gone — drop job. */
                        return REND_SUCCESS;
                }
                ref_put(&reply_port->refcount, free_message_port_ref);

                md = kmsg_create(module, resp_opcode, rfmt, job->result);
                if (!md)
                        return -E_RENDEZVOS;
                e = ipc_rpc_coop_enqueue_kmsg(md);
                if (e != REND_SUCCESS)
                        return e;
                job->reply_payload_queued = true;
                job->q->send_owner = job;
        }

        reply_port = thread_lookup_port(job->reply_port);
        if (!reply_port) {
                ipc_rpc_coop_clear_send_owner(job->q, job);
                return REND_SUCCESS;
        }

        e = ipc_try_send_msg(reply_port);
        ref_put(&reply_port->refcount, free_message_port_ref);
        if (e == REND_SUCCESS || e == -E_REND_PORT_CLOSED) {
                job->reply_payload_queued = false;
                job->q->send_owner = NULL;
                return REND_SUCCESS;
        }
        if (e == -E_REND_AGAIN)
                return -E_REND_AGAIN;
        if (e == -E_REND_NO_MSG) {
                ipc_rpc_coop_clear_send_owner(job->q, job);
                return -E_REND_NO_MSG;
        }
        ipc_rpc_coop_clear_send_owner(job->q, job);
        return e;
}

static void ipc_rpc_coop_poll_nested_recv(ipc_rpc_coop_job_t* job)
{
        error_t e;
        Message_t* msg;
        const kmsg_t* km;
        i64 nested_result = 0;
        const char* rfmt;

        if (!job || job->state != IPC_RPC_COOP_ST_NESTED_RECV
            || !job->nested_reply)
                return;

        e = ipc_try_recv_msg(job->nested_reply);
        if (e == -E_REND_AGAIN)
                return;
        if (e == -E_REND_PORT_CLOSED) {
                ipc_rpc_coop_nested_release_port(job);
                job->state = IPC_RPC_COOP_ST_NONE;
                ipc_rpc_coop_job_set_result(job, -LINUX_EIO);
                return;
        }
        if (e != REND_SUCCESS)
                return;

        msg = dequeue_recv_msg();
        if (!msg)
                return;

        if (linux_ipc_kmsg_is_port_closed(job->nested_reply, msg)) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                ipc_rpc_coop_nested_release_port(job);
                job->state = IPC_RPC_COOP_ST_NONE;
                ipc_rpc_coop_job_set_result(job, -LINUX_EIO);
                return;
        }
        if (ipc_rpc_recv_is_interrupt(job->nested_reply, msg)) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                return;
        }

        km = kmsg_from_msg(msg);
        rfmt = job->nested_resp_fmt[0] ? job->nested_resp_fmt :
                                         IPC_RPC_RESP_FMT_DEFAULT;
        if (!km || km->hdr.opcode != job->nested_resp_opcode
            || ipc_serial_decode(
                       km->payload, km->hdr.payload_len, rfmt, &nested_result)
                       != REND_SUCCESS) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                ipc_rpc_coop_nested_release_port(job);
                job->state = IPC_RPC_COOP_ST_NONE;
                ipc_rpc_coop_job_set_result(job, -LINUX_EIO);
                return;
        }

        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
        job->nested_result = nested_result;
        ipc_rpc_coop_nested_release_port(job);
        job->state = IPC_RPC_COOP_ST_NONE;

        if (job->q && job->q->resume_fn)
                job->q->resume_fn(job, nested_result, job->q->resume_ctx);
        else
                ipc_rpc_coop_job_set_result(job, nested_result);
}

void ipc_rpc_coop_queue_poll(ipc_rpc_coop_queue_t* q, u16 module,
                             u16 resp_opcode, const char* resp_fmt)
{
        struct list_entry* pos;
        struct list_entry* n;

        if (!q || !q->inited)
                return;

        list_for_each_safe(pos, n, &q->jobs)
        {
                ipc_rpc_coop_job_t* job =
                        list_entry(pos, ipc_rpc_coop_job_t, node);
                error_t e;

                if (job->state == IPC_RPC_COOP_ST_NESTED_SEND) {
                        Message_Port_t* server;

                        if (q->send_owner != job)
                                continue;
                        server = thread_lookup_port(job->nested_server);
                        if (!server) {
                                ipc_rpc_coop_clear_send_owner(q, job);
                                ipc_rpc_coop_nested_release_port(job);
                                job->state = IPC_RPC_COOP_ST_NONE;
                                ipc_rpc_coop_job_set_result(job, -LINUX_EIO);
                                continue;
                        }
                        e = ipc_try_send_msg(server);
                        ref_put(&server->refcount, free_message_port_ref);
                        if (e == REND_SUCCESS) {
                                job->reply_payload_queued = false;
                                q->send_owner = NULL;
                                job->state = IPC_RPC_COOP_ST_NESTED_RECV;
                        } else if (e != -E_REND_AGAIN) {
                                ipc_rpc_coop_clear_send_owner(q, job);
                                ipc_rpc_coop_nested_release_port(job);
                                job->state = IPC_RPC_COOP_ST_NONE;
                                ipc_rpc_coop_job_set_result(job, -LINUX_EIO);
                        }
                        continue;
                }

                if (job->state == IPC_RPC_COOP_ST_NESTED_RECV) {
                        ipc_rpc_coop_poll_nested_recv(job);
                        continue;
                }

                if (job->state == IPC_RPC_COOP_ST_NEED_REPLY) {
                        e = ipc_rpc_coop_try_client_reply(
                                job, module, resp_opcode, resp_fmt);
                        if (e == REND_SUCCESS)
                                ipc_rpc_coop_job_release(job);
                        /* AGAIN / NO_MSG: keep for next poll */
                }
        }
}

void ipc_rpc_coop_server_loop(const char* listen_port_name, u16 service_id,
                              u16 resp_opcode, const char* resp_fmt,
                              ipc_rpc_coop_handler_t handler,
                              ipc_rpc_coop_queue_t* q,
                              ipc_server_poll_fn_t poll_extra,
                              void* poll_extra_ctx)
{
        Message_Port_t* port;

        if (!listen_port_name || !handler || !q)
                return;

        if (!q->inited)
                ipc_rpc_coop_queue_init(q);

        port = ipc_server_coop_lookup(listen_port_name);
        pr_info("[IPC-RPC] coop server loop on '%s' service_id=%u\n",
                listen_port_name,
                service_id);

        while (1) {
                error_t ret;
                bool parked;

                ipc_rpc_coop_queue_poll(q, service_id, resp_opcode, resp_fmt);
                parked = !list_empty(&q->jobs);
                if (poll_extra)
                        parked = poll_extra(poll_extra_ctx) || parked;

                ret = ipc_try_recv_msg(port);
                if (ret == -E_REND_PORT_CLOSED) {
                        ref_put(&port->refcount, free_message_port_ref);
                        port = ipc_server_coop_lookup(listen_port_name);
                        continue;
                }
                if (ret != REND_SUCCESS) {
                        if (parked) {
                                schedule(percpu(core_tm));
                                continue;
                        }
                        ret = recv_msg(port);
                        if (ret == -E_REND_PORT_CLOSED) {
                                ref_put(&port->refcount, free_message_port_ref);
                                port = ipc_server_coop_lookup(listen_port_name);
                                continue;
                        }
                        if (ret != REND_SUCCESS)
                                continue;
                }

                while (1) {
                        Message_t* msg = dequeue_recv_msg();
                        const kmsg_t* km;
                        const char* reply_name;
                        ipc_rpc_coop_job_t* job;
                        i64 result = 0;
                        ipc_rpc_coop_disp_t disp;

                        if (!msg)
                                break;

                        if (linux_ipc_kmsg_is_port_closed(port, msg)) {
                                ref_put(&msg->ms_queue_node.refcount,
                                        free_message_ref);
                                ref_put(&port->refcount, free_message_port_ref);
                                port = ipc_server_coop_lookup(listen_port_name);
                                break;
                        }

                        km = kmsg_from_msg(msg);
                        if (!km || km->hdr.module != service_id) {
                                ipc_rpc_reply(km,
                                              NULL,
                                              service_id,
                                              resp_opcode,
                                              resp_fmt,
                                              -LINUX_EIO);
                                ref_put(&msg->ms_queue_node.refcount,
                                        free_message_ref);
                                continue;
                        }

                        reply_name = ipc_serial_payload_reply_port(
                                km->payload, km->hdr.payload_len);
                        job = ipc_rpc_coop_job_create(q, reply_name);
                        if (!job) {
                                ipc_rpc_reply(km,
                                              reply_name,
                                              service_id,
                                              resp_opcode,
                                              resp_fmt,
                                              -LINUX_ENOMEM);
                                ref_put(&msg->ms_queue_node.refcount,
                                        free_message_ref);
                                continue;
                        }

                        disp = handler(job, km->hdr.opcode, km, &result);
                        if (disp == IPC_RPC_COOP_HANDLED) {
                                if (job->state != IPC_RPC_COOP_ST_NEED_REPLY)
                                        ipc_rpc_coop_job_set_result(job,
                                                                    result);
                                /*
                                 * Try progress immediately so the common
                                 * case (client already in recv) completes
                                 * without waiting for another listen wake.
                                 */
                                ipc_rpc_coop_queue_poll(
                                        q, service_id, resp_opcode, resp_fmt);
                        }
                        /* PARKED: job stays until resume / set_result */

                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                }
        }
}
