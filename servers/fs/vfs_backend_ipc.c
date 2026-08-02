#include "vfs_backend_ipc.h"

#include <common/refcount.h>
#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/ipc/port_naming.h>
#include <linux_compat/ipc/rpc.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/ipc/ipc_serial.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/thread_loader.h>

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
                err = ipc_serial_decode(km->payload, km->hdr.payload_len, "spt",
                                        &path, &req.ino_out, reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_LOOKUP;
                req.path = path;
                break;
        case VFS_BACKEND_IPC_OPC_READ:
                err = ipc_serial_decode(km->payload, km->hdr.payload_len, "pqqpt",
                                        &req.ino, &req.offset, &req.len, &req.buf,
                                        reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_READ;
                break;
        case VFS_BACKEND_IPC_OPC_WRITE:
                err = ipc_serial_decode(km->payload, km->hdr.payload_len, "pqqpt",
                                        &req.ino, &req.offset, &req.len,
                                        (void **)&req.wbuf, reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_WRITE;
                break;
        case VFS_BACKEND_IPC_OPC_TRUNCATE:
                err = ipc_serial_decode(km->payload, km->hdr.payload_len, "pqt",
                                        &req.ino, &req.size_arg, reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_TRUNCATE;
                break;
        case VFS_BACKEND_IPC_OPC_FLUSH:
                err = ipc_serial_decode(km->payload, km->hdr.payload_len, "pt",
                                        &req.ino, reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_FLUSH;
                break;
        case VFS_BACKEND_IPC_OPC_READDIR:
                err = ipc_serial_decode(km->payload, km->hdr.payload_len, "sqpt",
                                        &path, &req.dir_index, &req.dirent_out,
                                        reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_READDIR;
                req.path = path;
                break;
        case VFS_BACKEND_IPC_OPC_READLINK:
                err = ipc_serial_decode(km->payload, km->hdr.payload_len, "spt",
                                        &path, &req.readlink_buf,
                                        &req.readlink_cap, reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_READLINK;
                req.path = path;
                break;
        case VFS_BACKEND_IPC_OPC_MKDIR:
                err = ipc_serial_decode(km->payload, km->hdr.payload_len, "sut",
                                        &path, &req.mode_arg, reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_MKDIR;
                req.path = path;
                break;
        case VFS_BACKEND_IPC_OPC_CREATE:
                err = ipc_serial_decode(km->payload, km->hdr.payload_len, "sut",
                                        &path, &req.mode_arg, reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_CREATE;
                req.path = path;
                break;
        case VFS_BACKEND_IPC_OPC_UNLINK:
                err = ipc_serial_decode(km->payload, km->hdr.payload_len, "st",
                                        &path, reply_port_out);
                if (err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                req.op = VFS_BACKEND_OP_UNLINK;
                req.path = path;
                break;
        case VFS_BACKEND_IPC_OPC_RENAME: {
                const char *path2;

                err = ipc_serial_decode(km->payload, km->hdr.payload_len, "sst",
                                        &path, &path2, reply_port_out);
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

                err = ipc_serial_decode(km->payload, km->hdr.payload_len, "sst",
                                        &path, &path2, reply_port_out);
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

i64 vfs_backend_ipc_call(vfs_backend_req_t *req)
{
        Message_Port_t *reply;
        const char *port;
        u16 opc;
        i64 ret;

        if (!req) {
                return -LINUX_EINVAL;
        }

        port = req->port;
        if (!port) {
                return -LINUX_EINVAL;
        }

        opc = vfs_backend_ipc_opcode(req->op);
        if (opc == 0) {
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


        /*
         * Nested VFS→backend: uninterruptible so a signal cannot abort mid-I/O
         * and wedge the single listen thread relative to the backend.
         */
        switch (req->op) {
        case VFS_BACKEND_OP_LOOKUP:
                ret = ipc_rpc_call_named_uninterruptible(
                        port, reply, opc, "sp", req->path, req->ino_out);
                break;
        case VFS_BACKEND_OP_READ:
                ret = ipc_rpc_call_named_uninterruptible(port, reply, opc,
                                                         "pqqp", req->ino,
                                                         req->offset, req->len,
                                                         req->buf);
                break;
        case VFS_BACKEND_OP_WRITE:
                ret = ipc_rpc_call_named_uninterruptible(port, reply, opc,
                                                         "pqqp", req->ino,
                                                         req->offset, req->len,
                                                         req->wbuf);
                break;
        case VFS_BACKEND_OP_TRUNCATE:
                ret = ipc_rpc_call_named_uninterruptible(
                        port, reply, opc, "pq", req->ino, req->size_arg);
                break;
        case VFS_BACKEND_OP_FLUSH:
                ret = ipc_rpc_call_named_uninterruptible(port, reply, opc, "p",
                                                         req->ino);
                break;
        case VFS_BACKEND_OP_READDIR:
                ret = ipc_rpc_call_named_uninterruptible(
                        port, reply, opc, "sqp", req->path, req->dir_index,
                        req->dirent_out);
                break;
        case VFS_BACKEND_OP_READLINK:
                ret = ipc_rpc_call_named_uninterruptible(
                        port, reply, opc, "spq", req->path, req->readlink_buf,
                        req->readlink_cap);
                break;
        case VFS_BACKEND_OP_MKDIR:
                ret = ipc_rpc_call_named_uninterruptible(
                        port, reply, opc, "su", req->path, (u64)req->mode_arg);
                break;
        case VFS_BACKEND_OP_CREATE:
                ret = ipc_rpc_call_named_uninterruptible(
                        port, reply, opc, "su", req->path, (u64)req->mode_arg);
                break;
        case VFS_BACKEND_OP_UNLINK:
                ret = ipc_rpc_call_named_uninterruptible(port, reply, opc, "s",
                                                         req->path);
                break;
        case VFS_BACKEND_OP_RENAME:
                ret = ipc_rpc_call_named_uninterruptible(
                        port, reply, opc, "ss", req->path, req->path2);
                break;
        case VFS_BACKEND_OP_LINK:
                ret = ipc_rpc_call_named_uninterruptible(
                        port, reply, opc, "ss", req->path, req->path2);
                break;
        default:
                ret = -LINUX_EINVAL;
                break;
        }

        req->result = ret;
        ref_put(&reply->refcount, free_message_port_ref);
        return ret;
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
