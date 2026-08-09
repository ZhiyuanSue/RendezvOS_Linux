#include "vfs_backend_ipc.h"

#include <common/refcount.h>
#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/initcall.h>
#include <linux_compat/ipc/port_naming.h>
#include <linux_compat/ipc/rpc.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/ipc/ipc_serial.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/thread_loader.h>

#include "vfs.h"

extern struct Port_Table *global_port_table;

/* Append decimal digits of @val; return new length or 0 on overflow. */
static size_t vfs_be_append_u32(char *buf, size_t bufsize, size_t n, u32 val)
{
        char digits[12];
        u32 nd = 0;
        u32 tmp = val;
        u32 i;

        if (!buf || n >= bufsize)
                return 0;
        if (tmp == 0) {
                digits[nd++] = '0';
        } else {
                while (tmp && nd < sizeof(digits)) {
                        digits[nd++] = (char)('0' + (tmp % 10u));
                        tmp /= 10u;
                }
        }
        if (n + nd >= bufsize)
                return 0;
        for (i = 0; i < nd; i++)
                buf[n + i] = digits[nd - 1u - i];
        buf[n + nd] = '\0';
        return n + nd;
}

/*
 * Kernel-side VFS client reply: vfs_cli_k_<tag> (PORT_NAMING §3.2 sentinel).
 * tag must be unique among concurrent callers.
 */
static Message_Port_t *vfs_backend_ipc_cli_port(const char *tag)
{
        char name[PORT_NAME_LEN_MAX];
        size_t i = 0;
        size_t j;
        const char *pfx = VFS_CLIENT_PORT_PREFIX; /* "vfs_cli_" */

        if (!tag || !tag[0]) {
                return NULL;
        }

        while (pfx[i] && i + 1 < sizeof(name)) {
                name[i] = pfx[i];
                i++;
        }
        if (i + 2 >= sizeof(name)) {
                return NULL;
        }
        name[i++] = 'k';
        name[i++] = '_';
        for (j = 0; tag[j] && i + 1 < sizeof(name); j++) {
                name[i++] = tag[j];
        }
        name[i] = '\0';
        if (tag[j]) {
                return NULL;
        }

        return ipc_rpc_port_lookup_or_create(name);
}

/*
 * Per-thread reply tag. Shared "srv" raced when vfs_server blocked in
 * backend recv and a syscall-context kern load (execve) reused the same
 * port — hang before test START after ash copy_thread (DUMP: schedule +
 * ipc_port_try_match).
 */
static Message_Port_t *vfs_backend_ipc_thread_reply_port(void)
{
        char tag[24];
        Thread_Base *self = get_cpu_current_thread();
        u32 tid = self ? (u32)self->tid : 0;
        size_t n = 0;

        tag[0] = 't';
        n = 1;
        n = vfs_be_append_u32(tag, sizeof(tag), n, tid);
        if (n == 0)
                return NULL;
        return vfs_backend_ipc_cli_port(tag);
}

i64 vfs_backend_ipc_rpc_handler(u16 opcode, const kmsg_t *km,
                                char **reply_port_out,
                                vfs_backend_service_fn service)
{
        vfs_backend_req_t req;
        const char *path;
        error_t err;

        if (!km || !reply_port_out || !service) {
                return -LINUX_EINVAL;
        }

        memset(&req, 0, sizeof(req));

        switch (opcode) {
        case VFS_BACKEND_IPC_OPC_LOOKUP:
                err = ipc_serial_decode(km->payload,
                                        km->hdr.payload_len,
                                        "spt",
                                        &path,
                                        &req.ino_out,
                                        reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_LOOKUP;
                req.path = path;
                break;
        case VFS_BACKEND_IPC_OPC_READ:
                err = ipc_serial_decode(km->payload,
                                        km->hdr.payload_len,
                                        "pqqpt",
                                        &req.ino,
                                        &req.offset,
                                        &req.len,
                                        &req.buf,
                                        reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_READ;
                break;
        case VFS_BACKEND_IPC_OPC_WRITE:
                err = ipc_serial_decode(km->payload,
                                        km->hdr.payload_len,
                                        "pqqpt",
                                        &req.ino,
                                        &req.offset,
                                        &req.len,
                                        (void **)&req.wbuf,
                                        reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_WRITE;
                break;
        case VFS_BACKEND_IPC_OPC_TRUNCATE:
                err = ipc_serial_decode(km->payload,
                                        km->hdr.payload_len,
                                        "pqt",
                                        &req.ino,
                                        &req.size_arg,
                                        reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_TRUNCATE;
                break;
        case VFS_BACKEND_IPC_OPC_FLUSH:
                err = ipc_serial_decode(km->payload,
                                        km->hdr.payload_len,
                                        "pt",
                                        &req.ino,
                                        reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_FLUSH;
                break;
        case VFS_BACKEND_IPC_OPC_READDIR:
                err = ipc_serial_decode(km->payload,
                                        km->hdr.payload_len,
                                        "sqpt",
                                        &path,
                                        &req.dir_index,
                                        &req.dirent_out,
                                        reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_READDIR;
                req.path = path;
                break;
        case VFS_BACKEND_IPC_OPC_READLINK:
                err = ipc_serial_decode(km->payload,
                                        km->hdr.payload_len,
                                        "spt",
                                        &path,
                                        &req.readlink_buf,
                                        &req.readlink_cap,
                                        reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_READLINK;
                req.path = path;
                break;
        case VFS_BACKEND_IPC_OPC_MKDIR:
                err = ipc_serial_decode(km->payload,
                                        km->hdr.payload_len,
                                        "sut",
                                        &path,
                                        &req.mode_arg,
                                        reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_MKDIR;
                req.path = path;
                break;
        case VFS_BACKEND_IPC_OPC_CREATE:
                err = ipc_serial_decode(km->payload,
                                        km->hdr.payload_len,
                                        "sut",
                                        &path,
                                        &req.mode_arg,
                                        reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_CREATE;
                req.path = path;
                break;
        case VFS_BACKEND_IPC_OPC_UNLINK:
                err = ipc_serial_decode(km->payload,
                                        km->hdr.payload_len,
                                        "st",
                                        &path,
                                        reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_UNLINK;
                req.path = path;
                break;
        case VFS_BACKEND_IPC_OPC_RENAME: {
                const char *path2;

                err = ipc_serial_decode(km->payload,
                                        km->hdr.payload_len,
                                        "sst",
                                        &path,
                                        &path2,
                                        reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_RENAME;
                req.path = path;
                req.path2 = path2;
                break;
        }
        case VFS_BACKEND_IPC_OPC_LINK: {
                const char *path2;

                err = ipc_serial_decode(km->payload,
                                        km->hdr.payload_len,
                                        "sst",
                                        &path,
                                        &path2,
                                        reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_LINK;
                req.path = path;
                req.path2 = path2;
                break;
        }
        default:
                return -LINUX_ENOSYS;
        }

        return service(&req);
}

error_t vfs_backend_ipc_server_spawn(const char *port_name,
                                     const char *thread_name,
                                     u16 *service_id_out,
                                     Thread_Base **thread_out,
                                     void (*thread_entry)(void))
{
        Message_Port_t *port;
        error_t err;

        if (!port_name || !thread_name || !service_id_out || !thread_out
            || !thread_entry || !global_port_table) {
                return -E_IN_PARAM;
        }

        port = create_message_port(port_name);
        if (!port) {
                return -E_REND_NO_MEM;
        }

        register_port(global_port_table, port);
        *service_id_out = port->service_id;

        err = gen_thread_from_func(thread_out,
                                   (kthread_func)thread_entry,
                                   (char *)thread_name,
                                   percpu(core_tm),
                                   NULL);
        if (err != REND_SUCCESS) {
                return err;
        }

        return REND_SUCCESS;
}

static u16 vfs_backend_ipc_opcode(vfs_backend_op_t op)
{
        switch (op) {
        case VFS_BACKEND_OP_LOOKUP:
                return VFS_BACKEND_IPC_OPC_LOOKUP;
        case VFS_BACKEND_OP_READ:
                return VFS_BACKEND_IPC_OPC_READ;
        case VFS_BACKEND_OP_WRITE:
                return VFS_BACKEND_IPC_OPC_WRITE;
        case VFS_BACKEND_OP_TRUNCATE:
                return VFS_BACKEND_IPC_OPC_TRUNCATE;
        case VFS_BACKEND_OP_FLUSH:
                return VFS_BACKEND_IPC_OPC_FLUSH;
        case VFS_BACKEND_OP_READDIR:
                return VFS_BACKEND_IPC_OPC_READDIR;
        case VFS_BACKEND_OP_READLINK:
                return VFS_BACKEND_IPC_OPC_READLINK;
        case VFS_BACKEND_OP_MKDIR:
                return VFS_BACKEND_IPC_OPC_MKDIR;
        case VFS_BACKEND_OP_CREATE:
                return VFS_BACKEND_IPC_OPC_CREATE;
        case VFS_BACKEND_OP_UNLINK:
                return VFS_BACKEND_IPC_OPC_UNLINK;
        case VFS_BACKEND_OP_RENAME:
                return VFS_BACKEND_IPC_OPC_RENAME;
        case VFS_BACKEND_OP_LINK:
                return VFS_BACKEND_IPC_OPC_LINK;
        default:
                return 0;
        }
}

/*
 * Shared opcode→args mapping. EMIT(fmt, ...) expands differently for
 * coop nested (return nested_call) vs sync (assign + goto out).
 */
#define VFS_BE_CASES(EMIT)                                                     \
        case VFS_BACKEND_OP_LOOKUP:                                            \
                EMIT("sp", req->path, req->ino_out);                           \
        case VFS_BACKEND_OP_READ:                                              \
                EMIT("pqqp", req->ino, req->offset, req->len, req->buf);       \
        case VFS_BACKEND_OP_WRITE:                                             \
                EMIT("pqqp", req->ino, req->offset, req->len, req->wbuf);      \
        case VFS_BACKEND_OP_TRUNCATE:                                          \
                EMIT("pq", req->ino, req->size_arg);                           \
        case VFS_BACKEND_OP_FLUSH:                                             \
                EMIT("p", req->ino);                                           \
        case VFS_BACKEND_OP_READDIR:                                           \
                EMIT("sqp", req->path, req->dir_index, req->dirent_out);       \
        case VFS_BACKEND_OP_READLINK:                                          \
                EMIT("spq",                                                    \
                     req->path,                                                \
                     req->readlink_buf,                                        \
                     req->readlink_cap);                                       \
        case VFS_BACKEND_OP_MKDIR:                                             \
        case VFS_BACKEND_OP_CREATE:                                            \
                EMIT("su", req->path, (u64)req->mode_arg);                     \
        case VFS_BACKEND_OP_UNLINK:                                            \
                EMIT("s", req->path);                                          \
        case VFS_BACKEND_OP_RENAME:                                            \
        case VFS_BACKEND_OP_LINK:                                              \
                EMIT("ss", req->path, req->path2)

error_t vfs_backend_ipc_coop_nested(ipc_rpc_coop_job_t *job,
                                    vfs_backend_req_t *req)
{
        const char *port;
        u16 opc;

        if (!job || !req || !req->port) {
                return -E_IN_PARAM;
        }

        port = req->port;
        opc = vfs_backend_ipc_opcode(req->op);
        if (opc == 0) {
                return -E_IN_PARAM;
        }

#define VFS_BE_EMIT(fmt, ...)                                                  \
        return ipc_rpc_coop_nested_call(job, port, opc, fmt, __VA_ARGS__)

        switch (req->op) {
                VFS_BE_CASES(VFS_BE_EMIT);
        default:
                return -E_IN_PARAM;
        }
#undef VFS_BE_EMIT
}

i64 vfs_backend_ipc_call(vfs_backend_req_t *req)
{
        Message_Port_t *reply;
        i64 ret;
        const char *port;
        u16 opc;

        if (!req || !req->port) {
                return -LINUX_EINVAL;
        }

        reply = vfs_backend_ipc_thread_reply_port();
        if (!reply) {
                Thread_Base *self = get_cpu_current_thread();

                pr_error("[vfs-be] reply port alloc failed tid=%d op=%d\n",
                         self ? (int)self->tid : -1,
                         (int)req->op);
                return -LINUX_ENOMEM;
        }

        port = req->port;
        opc = vfs_backend_ipc_opcode(req->op);
        if (opc == 0) {
                ref_put(&reply->refcount, free_message_port_ref);
                return -LINUX_EINVAL;
        }

#define VFS_BE_EMIT(fmt, ...)                                                  \
        do {                                                                   \
                ret = ipc_rpc_call_named_uninterruptible(                      \
                        port, reply, opc, fmt, __VA_ARGS__);                   \
                goto out;                                                      \
        } while (0)

        /* Blocking path (exec / remaining sync ops). Listen uses coop_nested. */
        switch (req->op) {
                VFS_BE_CASES(VFS_BE_EMIT);
        default:
                ret = -LINUX_EINVAL;
                break;
        }
#undef VFS_BE_EMIT

out:
        req->result = ret;
        ref_put(&reply->refcount, free_message_port_ref);
        return ret;
}

#undef VFS_BE_CASES

static ipc_rpc_coop_disp_t vfs_backend_ipc_coop_adapt(ipc_rpc_coop_job_t *job,
                                                      u16 opcode,
                                                      const kmsg_t *km,
                                                      i64 *result_out)
{
        vfs_backend_service_fn service;
        char *rp = NULL;
        i64 result;

        if (!job || !job->q || !result_out || !km) {
                return IPC_RPC_COOP_HANDLED;
        }

        service = (vfs_backend_service_fn)job->q->resume_ctx;
        if (!service) {
                *result_out = -LINUX_EIO;
                return IPC_RPC_COOP_HANDLED;
        }

        /*
         * Nested VFS callers park in NESTED_RECV and drain recv_msg_queue
         * (coop poll). Reply with enqueue + ipc_transfer_message to the VFS
         * listen thread — no reply-port rendezvous, no invented wake.
         *
         * Sync kern callers still use a real vfs_cli_k_t* port and blocking
         * ipc_rpc_reply (they block in recv_msg on that port).
         */
        result = vfs_backend_ipc_rpc_handler(opcode, km, &rp, service);
        *result_out = result;

        {
                const char *reply = (rp && rp[0]) ? rp : job->reply_port;

                if (ipc_rpc_is_nest_token(reply)) {
                        Thread_Base *vfs = vfs_server_thread_get();
                        u64 cookie = ipc_rpc_nest_token_cookie(reply);

                        if (!vfs
                            || !ipc_rpc_nest_reply_transfer(vfs, cookie,
                                                            result)) {
                                pr_error("[vfs-be] nest reply transfer failed "
                                         "cookie=%llu result=%ld\n",
                                         (unsigned long long)cookie,
                                         result);
                        }
                        return IPC_RPC_COOP_REPLIED;
                }

                ipc_rpc_reply(km,
                              reply,
                              km->hdr.module,
                              IPC_RPC_RESP_OPCODE_DEFAULT,
                              IPC_RPC_RESP_FMT_DEFAULT,
                              result);
                return IPC_RPC_COOP_REPLIED;
        }
}

void vfs_backend_ipc_coop_server_loop(const char *listen_port_name,
                                      u16 service_id,
                                      ipc_rpc_coop_queue_t *q,
                                      vfs_backend_service_fn service)
{
        if (!listen_port_name || !q || !service) {
                return;
        }

        if (!q->inited) {
                ipc_rpc_coop_queue_init(q);
        }
        /* Store service in resume_ctx (no nested resume for leaves). */
        ipc_rpc_coop_queue_set_resume(q, NULL, (void *)service);

        ipc_rpc_coop_server_loop(listen_port_name,
                                 service_id,
                                 IPC_RPC_RESP_OPCODE_DEFAULT,
                                 IPC_RPC_RESP_FMT_DEFAULT,
                                 vfs_backend_ipc_coop_adapt,
                                 q,
                                 NULL,
                                 NULL);
}

i64 vfs_backend_ipc_leaf_run(const char *port_name, const char *fstype,
                             u32 caps, u32 reg_flags, u16 service_id,
                             ipc_rpc_coop_queue_t *q,
                             vfs_backend_service_fn service)
{
        i64 reg_ret;

        if (!port_name || !q || !service) {
                return -LINUX_EINVAL;
        }

        reg_ret = vfs_backend_ipc_register(port_name, fstype, caps, reg_flags);
        if (reg_ret < 0) {
                return reg_ret;
        }
        if (reg_flags) {
                vfs_backend_mark_online(reg_flags);
        }
        vfs_backend_ipc_coop_server_loop(port_name, service_id, q, service);
        return 0;
}

error_t vfs_backend_ipc_leaf_spawn(const char *port_name,
                                   const char *thread_name,
                                   const char *log_tag, u16 *service_id_out,
                                   Thread_Base **thread_out,
                                   void (*thread_entry)(void),
                                   bool *once_done)
{
        error_t err;

        if (!port_name || !thread_name || !log_tag || !service_id_out
            || !thread_out || !thread_entry || !once_done) {
                return -E_IN_PARAM;
        }
        if (!linux_init_vfs_service_once(once_done)) {
                return REND_SUCCESS;
        }

        err = vfs_backend_ipc_server_spawn(port_name,
                                           thread_name,
                                           service_id_out,
                                           thread_out,
                                           thread_entry);
        if (err != REND_SUCCESS) {
                pr_error("[VFS/%s] server spawn failed: %d on CPU %llu\n",
                         log_tag,
                         (int)err,
                         (u64)percpu(cpu_number));
                return err;
        }

        pr_info("[VFS/%s] backend thread on CPU %llu port '%s'\n",
                log_tag,
                (u64)percpu(cpu_number),
                port_name);
        linux_init_vfs_service_mark_done(once_done);
        return REND_SUCCESS;
}

i64 vfs_backend_ipc_register(const char *port_name, const char *fstype,
                             u32 caps, u32 reg_flags)
{
        Message_Port_t *reply;
        char tag[VFS_BACKEND_FSTYPE_MAX + 4];
        size_t i;
        size_t j;
        const char *ft;
        i64 ret;

        if (!port_name || !port_name[0]) {
                return -LINUX_EINVAL;
        }

        /*
         * Backends register concurrently at init: each needs its own reply
         * port (vfs_cli_k_reg_<fstype>), not a shared singleton.
         */
        ft = fstype && fstype[0] ? fstype : "anon";
        i = 0;
        tag[i++] = 'r';
        tag[i++] = 'e';
        tag[i++] = 'g';
        tag[i++] = '_';
        for (j = 0; ft[j] && i + 1 < sizeof(tag); j++) {
                tag[i++] = ft[j];
        }
        tag[i] = '\0';

        reply = vfs_backend_ipc_cli_port(tag);
        if (!reply) {
                return -LINUX_ENOMEM;
        }

        /* VFS listen is single-threaded; never abandon after send. */
        ret = ipc_rpc_call_named_uninterruptible(VFS_SERVER_PORT_NAME,
                                                 reply,
                                                 KMSG_OP_VFS_BACKEND_REGISTER,
                                                 VFS_KMSG_FMT_BACKEND_REGISTER,
                                                 port_name,
                                                 fstype ? fstype : "",
                                                 (u64)caps,
                                                 (u64)reg_flags);

        ref_put(&reply->refcount, free_message_port_ref);
        return ret;
}
