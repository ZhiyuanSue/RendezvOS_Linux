/*
 * linux_compat IPC RPC framework (request–reply + one-way server loops).
 */

#include <common/string.h>
#include <linux_compat/debug_trace.h>
#include <linux_compat/errno.h>
#include <linux_compat/ipc/block_wake.h>
#include <linux_compat/ipc/port_naming.h>
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

#if LINUX_COMPAT_TRACE_IPC_REPLY_STALL
/*
 * Trace reply rendezvous on:
 *   vfs_cli_k_t*  — nested VFS→backend (always when STALL=1)
 *   vfs_cli_*     — user→VFS when STALL_USER=1 (munmap wedge)
 */
static bool ipc_rpc_trace_reply_port(const char* name)
{
        const char* kpfx = "vfs_cli_k_t";
        const char* upfx = "vfs_cli_";
        size_t i;

        if (!name) {
                return false;
        }
        for (i = 0; kpfx[i]; i++) {
                if (name[i] != kpfx[i]) {
                        break;
                }
        }
        if (!kpfx[i] && name[i] != '\0') {
                return true;
        }
#if LINUX_COMPAT_TRACE_IPC_REPLY_STALL_USER
        for (i = 0; upfx[i]; i++) {
                if (name[i] != upfx[i]) {
                        return false;
                }
        }
        /* Exclude k_reg_* register noise; keep vfs_cli_<pid>. */
        if (name[i] == 'k') {
                return false;
        }
        return name[i] != '\0';
#else
        (void)upfx;
        return false;
#endif
}

/*
 * Opcode namespaces collide (both start at SYSTEM_END+1). Pick by port:
 *   vfs_cli_k_t* → backend LOOKUP/READ/…
 *   vfs_cli_<pid> → VFS OPEN/READ/…
 */
static const char* ipc_rpc_stall_op_name(const char* port, u16 op)
{
        bool backend = false;
        const char* kpfx = "vfs_cli_k_t";
        size_t i;

        if (port) {
                for (i = 0; kpfx[i]; i++) {
                        if (port[i] != kpfx[i]) {
                                break;
                        }
                }
                backend = (!kpfx[i] && port[i] != '\0');
        }

        if (backend) {
                switch (op) {
                case 7:
                        return "BE_LOOKUP";
                case 8:
                        return "BE_READ";
                case 9:
                        return "BE_WRITE";
                case 10:
                        return "BE_TRUNC";
                case 11:
                        return "BE_FLUSH";
                case 15:
                        return "BE_CREATE";
                default:
                        return "BE_?";
                }
        }

        switch (op) {
        case 7:
                return "VFS_OPEN";
        case 8:
                return "VFS_CLOSE";
        case 9:
                return "VFS_READ";
        case 10:
                return "VFS_WRITE";
        case 11:
                return "VFS_FSTAT";
        case 13:
                return "VFS_LSEEK";
        case 29:
                return "VFS_READLINKAT";
        case 30:
                return "VFS_FACCESSAT";
        default:
                return "VFS_?";
        }
}

static u32 ipc_rpc_stall_seq;

static u32 ipc_rpc_stall_next_seq(void)
{
        return ++ipc_rpc_stall_seq;
}
#endif

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
        /* Drop creator ref; message holds its own (see create_message_with_msg). */
        ref_put(&msg_data->refcount, free_msgdata_ref_default);
        if (!msg) {
                return -LINUX_ENOMEM;
        }

        err = enqueue_msg_for_send(msg);
        if (err != REND_SUCCESS) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                return -LINUX_EIO;
        }

#if LINUX_COMPAT_TRACE_IPC_WEDGE
        {
                Thread_Base* self = get_cpu_current_thread();
                Tcb_Base* task = get_cpu_current_task();

                pr_info("[rpc] SEND srv='%s' reply='%s' op=%u tid=%d pid=%d\n",
                        server_port->name ? server_port->name : "?",
                        reply_port->name ? reply_port->name : "?",
                        (unsigned)req_opcode,
                        self ? (int)self->tid : -1,
                        task ? (int)task->pid : -1);
        }
#endif
        err = send_msg(server_port);
        if (err != REND_SUCCESS) {
#if LINUX_COMPAT_TRACE_IPC_WEDGE
                pr_error("[rpc] SEND fail srv='%s' err=%d\n",
                         server_port->name ? server_port->name : "?",
                         (int)err);
#endif
                /* Includes -E_REND_PORT_CLOSED if listen/reply port was closed. */
                return -LINUX_EIO;
        }

        /* Committed: wait for reply or reply-port close. Never -EINTR here. */
#if LINUX_COMPAT_TRACE_IPC_WEDGE
        pr_info("[rpc] RECV reply='%s' op=%u\n",
                reply_port->name ? reply_port->name : "?",
                (unsigned)req_opcode);
#endif
#if LINUX_COMPAT_TRACE_IPC_REPLY_STALL
        u32 stall_seq = 0;

        if (ipc_rpc_trace_reply_port(reply_port->name)) {
                Thread_Base* self = get_cpu_current_thread();

                stall_seq = ipc_rpc_stall_next_seq();
                pr_info("[rpc-stall] #%u recv_enter port='%s' op=%u(%s) "
                        "tid=%d cpu=%lu\n",
                        stall_seq,
                        reply_port->name,
                        (unsigned)req_opcode,
                        ipc_rpc_stall_op_name(reply_port->name, req_opcode),
                        self ? (int)self->tid : -1,
                        (u64)percpu(cpu_number));
        }
#endif
        for (;;) {
                err = recv_msg(reply_port);
                if (err == -E_REND_PORT_CLOSED) {
#if LINUX_COMPAT_TRACE_IPC_REPLY_STALL
                        if (stall_seq) {
                                pr_info("[rpc-stall] #%u recv_leave port='%s' "
                                        "PORT_CLOSED\n",
                                        stall_seq,
                                        reply_port->name);
                        }
#endif
                        ipc_rpc_drain_recv_queue();
                        return -LINUX_EIO;
                }
                if (err != REND_SUCCESS) {
                        /*
                         * Transient recv failure (incl. after XFER_FAIL wake
                         * from send-side NO_MSG) must not abandon — retry.
                         */
#if LINUX_COMPAT_TRACE_IPC_REPLY_STALL
                        if (stall_seq) {
                                Thread_Base* self = get_cpu_current_thread();

                                pr_info("[rpc-stall] #%u recv_retry port='%s' "
                                        "err=%d flags=0x%llx st=%lu tid=%d\n",
                                        stall_seq,
                                        reply_port->name,
                                        (int)err,
                                        self ? (unsigned long long)self->flags
                                             : 0ull,
                                        self ? thread_get_status(self) : 0ul,
                                        self ? (int)self->tid : -1);
                        }
#endif
                        ipc_rpc_drain_recv_queue();
                        schedule(percpu(core_tm));
                        continue;
                }

                msg = dequeue_recv_msg();
                if (!msg) {
#if LINUX_COMPAT_TRACE_IPC_REPLY_STALL
                        if (stall_seq) {
                                pr_info("[rpc-stall] #%u recv_empty port='%s' "
                                        "(SUCCESS but no msg)\n",
                                        stall_seq,
                                        reply_port->name);
                        }
#endif
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
#if LINUX_COMPAT_TRACE_IPC_REPLY_STALL
                if (stall_seq) {
                        pr_info("[rpc-stall] #%u recv_leave port='%s' ok "
                                "result=%ld\n",
                                stall_seq,
                                reply_port->name,
                                result);
                }
#endif
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
                                       u16 req_opcode, const char* req_fmt,
                                       ...)
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
 * Blocking rendezvous reply. Live clients are on (or soon on) recv in
 * ipc_rpc_call*. Abandoned clients: unregister_port → port_clean wakes
 * block_on_send with THREAD_FLAG_IPC_PORT_CLOSED; send_msg returns
 * -E_REND_PORT_CLOSED and drops the orphan send payload. Listen must treat
 * that as success-of-abandon and continue (not wedge). Do not use bare
 * try_send — races "server replies before client recv".
 *
 * Alloc/enqueue failures retry until the reply port disappears — giving up
 * would leave the client blocked forever in recv. After enqueue+send_msg
 * starts, do not retry (would duplicate the reply).
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
                        /* Unregistered — peer gone; listen must continue. */
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
                 * Payload is on this thread's send slot — send once.
                 * PORT_CLOSED: client tore down while we blocked; core dropped
                 * the orphan — treat as handled so listen continues.
                 */
#if LINUX_COMPAT_TRACE_IPC_WEDGE
                pr_info("[rpc] REPLY send reply='%s' result=%ld\n",
                        reply_port_name,
                        result);
#endif
#if LINUX_COMPAT_TRACE_IPC_REPLY_STALL
                u32 reply_seq = 0;

                if (ipc_rpc_trace_reply_port(reply_port_name)) {
                        Thread_Base* self = get_cpu_current_thread();

                        reply_seq = ipc_rpc_stall_next_seq();
                        pr_info("[rpc-stall] #%u reply_enter port='%s' "
                                "result=%ld tid=%d cpu=%lu flags=0x%llx "
                                "st=%lu\n",
                                reply_seq,
                                reply_port_name,
                                result,
                                self ? (int)self->tid : -1,
                                (u64)percpu(cpu_number),
                                self ? (unsigned long long)self->flags : 0ull,
                                self ? thread_get_status(self) : 0ul);
                }
#endif
                send_err = send_msg(reply_port);
                ref_put(&reply_port->refcount, free_message_port_ref);
#if LINUX_COMPAT_TRACE_IPC_REPLY_STALL
                if (reply_seq) {
                        Thread_Base* self = get_cpu_current_thread();

                        pr_info("[rpc-stall] #%u reply_leave port='%s' "
                                "err=%d flags=0x%llx st=%lu\n",
                                reply_seq,
                                reply_port_name,
                                (int)send_err,
                                self ? (unsigned long long)self->flags : 0ull,
                                self ? thread_get_status(self) : 0ul);
                }
#endif
                if (send_err == REND_SUCCESS
                    || send_err == -E_REND_PORT_CLOSED) {
#if LINUX_COMPAT_TRACE_IPC_WEDGE
                        if (send_err == -E_REND_PORT_CLOSED) {
                                pr_info("[rpc] REPLY PORT_CLOSED reply='%s'\n",
                                        reply_port_name);
                        }
#endif
                        return true;
                }
                /*
                 * -E_REND_NO_MSG: transfer abandoned; core woke the receiver
                 * with XFER_FAIL. Payload was not delivered — safe to rebuild
                 * and retry (not a duplicate).
                 */
                if (send_err == -E_REND_NO_MSG || send_err == -E_REND_AGAIN) {
#if LINUX_COMPAT_TRACE_IPC_REPLY_STALL
                        if (reply_seq) {
                                pr_info("[rpc-stall] #%u reply_retry port='%s' "
                                        "err=%d result=%ld\n",
                                        reply_seq,
                                        reply_port_name,
                                        (int)send_err,
                                        result);
                        }
#endif
                        pr_warn("[IPC-RPC] reply send err=%d port='%s' — "
                                "retry\n",
                                (int)send_err,
                                reply_port_name);
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

bool ipc_server_coop_flag_pending(void* ctx)
{
        bool* flag = (bool*)ctx;

        return flag && *flag;
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

        while (1) {
                error_t ret;
                bool still_pending = false;

                if (poll_pending) {
                        still_pending = poll_pending(poll_ctx);
                }

                ret = ipc_try_recv_msg(port);
                if (ret == REND_SUCCESS) {
                        ipc_server_coop_drain(&port,
                                              listen_port_name,
                                              on_message);
                        continue;
                }

                if (ret == -E_REND_PORT_CLOSED) {
                        ref_put(&port->refcount, free_message_port_ref);
                        port = ipc_server_coop_lookup(listen_port_name);
                        continue;
                }

                if (still_pending) {
                        schedule(percpu(core_tm));
                        continue;
                }

                ret = recv_msg(port);
                if (ret != REND_SUCCESS) {
                        if (ret == -E_REND_PORT_CLOSED) {
                                ref_put(&port->refcount,
                                        free_message_port_ref);
                                port = ipc_server_coop_lookup(
                                        listen_port_name);
                        } else {
                                schedule(percpu(core_tm));
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
