/*
 * linux_compat IPC RPC framework (request–reply + one-way server loops).
 */

#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/ipc/block_wake.h>
#include <linux_compat/ipc/rpc.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/signal/signal_deliver.h>
#include <modules/log/log.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/ipc_serial.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>

extern struct Port_Table* global_port_table;

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
        Message_t* stale;

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

i64 ipc_rpc_call_va(Message_Port_t* server_port, Message_Port_t* reply_port,
                    u16 req_opcode, const char* req_fmt, u16 resp_opcode,
                    const char* resp_fmt, va_list ap)
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

        module = server_port->service_id;
        ipc_rpc_drain_recv_queue();

        msg_data = ipc_kmsg_create_request(
                module, req_opcode, req_fmt, reply_port->name, ap);
        if (!msg_data) {
                return -LINUX_EINVAL;
        }

        msg = create_message_with_msg(msg_data);
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
                return -LINUX_EIO;
        }

        if (linux_signal_has_deliverable_pending()) {
                return -LINUX_EINTR;
        }

        err = recv_msg(reply_port);
        if (err != REND_SUCCESS) {
                ipc_rpc_drain_recv_queue();
                if (linux_signal_has_deliverable_pending()) {
                        return -LINUX_EINTR;
                }
                return -LINUX_EIO;
        }

        msg = dequeue_recv_msg();
        if (!msg) {
                if (linux_signal_has_deliverable_pending()) {
                        return -LINUX_EINTR;
                }
                return -LINUX_EIO;
        }

        if (ipc_rpc_recv_is_interrupt(reply_port, msg)) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                return -LINUX_EINTR;
        }

        if (linux_signal_has_deliverable_pending()) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                return -LINUX_EINTR;
        }

        {
                const kmsg_t* resp_kmsg = kmsg_from_msg(msg);
                if (!resp_kmsg || resp_kmsg->hdr.module != module
                    || resp_kmsg->hdr.opcode != resp_opcode) {
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                        return -LINUX_EIO;
                }

                err = ipc_serial_decode(resp_kmsg->payload,
                                        resp_kmsg->hdr.payload_len,
                                        rfmt,
                                        &result);
                if (err != REND_SUCCESS) {
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                        return -LINUX_EIO;
                }
        }

        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
        return result;
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

        reply_port = thread_lookup_port(reply_port_name);
        if (!reply_port) {
                return false;
        }

        resp_data = kmsg_create(module, resp_opcode, rfmt, result);
        if (!resp_data) {
                ref_put(&reply_port->refcount, free_message_port_ref);
                return false;
        }

        resp_msg = create_message_with_msg(resp_data);
        if (!resp_msg) {
                ref_put(&reply_port->refcount, free_message_port_ref);
                return false;
        }

        send_err = enqueue_msg_for_send(resp_msg);
        if (send_err == REND_SUCCESS) {
                send_err = send_msg(reply_port);
        }

        ref_put(&reply_port->refcount, free_message_port_ref);
        return send_err == REND_SUCCESS;
}

void ipc_rpc_reply_best_effort(const kmsg_t* km, const char* reply_port_name,
                               u16 module, u16 resp_opcode,
                               const char* resp_fmt, i64 result)
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
                pr_error("[IPC-RPC] reply failed port='%s' result=%ld\n",
                         reply_copy,
                         result);
        }
}

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
                                ipc_rpc_reply_best_effort(km,
                                                          NULL,
                                                          service_id,
                                                          resp_opcode,
                                                          resp_fmt,
                                                          -LINUX_EIO);
                                ref_put(&msg->ms_queue_node.refcount,
                                        free_message_ref);
                                continue;
                        }

                        result = handler(km->hdr.opcode, km, &reply_port);
                        ipc_rpc_reply_best_effort(km,
                                                  reply_port,
                                                  service_id,
                                                  resp_opcode,
                                                  resp_fmt,
                                                  result);
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                }
        }
}

void ipc_server_recv_loop(const char* listen_port_name,
                          ipc_server_message_fn_t on_message)
{
        Message_Port_t* port = NULL;

        if (!listen_port_name || !on_message) {
                return;
        }

        while (!port) {
                port = thread_lookup_port(listen_port_name);
                if (!port) {
                        schedule(percpu(core_tm));
                }
        }

        while (1) {
                error_t ret = recv_msg(port);
                u16 service_id = port->service_id;

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
                        if (!msg) {
                                break;
                        }
                        on_message(msg, service_id);
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                }
        }
}
