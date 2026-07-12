#include "vfs_backend_ipc.h"

#include <common/refcount.h>
#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/ipc/rpc.h>
#include <rendezvos/error.h>
#include <rendezvos/ipc/ipc_serial.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/thread_loader.h>

extern struct Port_Table *global_port_table;

static Message_Port_t *vfs_backend_ipc_reply_port(void)
{
        return ipc_rpc_port_lookup_or_create(VFS_BACKEND_IPC_CALLER_PORT);
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

        reply = vfs_backend_ipc_reply_port();
        if (!reply) {
                return -LINUX_ENOMEM;
        }

        switch (req->op) {
        case VFS_BACKEND_OP_LOOKUP:
                ret = ipc_rpc_call_named(port, reply, opc, "sp", req->path,
                                         req->ino_out);
                break;
        case VFS_BACKEND_OP_READ:
                ret = ipc_rpc_call_named(port, reply, opc, "pqqp", req->ino,
                                         req->offset, req->len, req->buf);
                break;
        case VFS_BACKEND_OP_WRITE:
                ret = ipc_rpc_call_named(port, reply, opc, "pqqp", req->ino,
                                         req->offset, req->len, req->wbuf);
                break;
        case VFS_BACKEND_OP_TRUNCATE:
                ret = ipc_rpc_call_named(port, reply, opc, "pq", req->ino,
                                         req->size_arg);
                break;
        case VFS_BACKEND_OP_FLUSH:
                ret = ipc_rpc_call_named(port, reply, opc, "p", req->ino);
                break;
        case VFS_BACKEND_OP_READDIR:
                ret = ipc_rpc_call_named(port, reply, opc, "sqp", req->path,
                                         req->dir_index, req->dirent_out);
                break;
        case VFS_BACKEND_OP_READLINK:
                ret = ipc_rpc_call_named(port, reply, opc, "spq", req->path,
                                         req->readlink_buf, req->readlink_cap);
                break;
        case VFS_BACKEND_OP_MKDIR:
                ret = ipc_rpc_call_named(port, reply, opc, "su", req->path,
                                         (u64)req->mode_arg);
                break;
        case VFS_BACKEND_OP_CREATE:
                ret = ipc_rpc_call_named(port, reply, opc, "su", req->path,
                                         (u64)req->mode_arg);
                break;
        case VFS_BACKEND_OP_UNLINK:
                ret = ipc_rpc_call_named(port, reply, opc, "s", req->path);
                break;
        case VFS_BACKEND_OP_RENAME:
                ret = ipc_rpc_call_named(port, reply, opc, "ss", req->path,
                                         req->path2);
                break;
        case VFS_BACKEND_OP_LINK:
                ret = ipc_rpc_call_named(port, reply, opc, "ss", req->path,
                                         req->path2);
                break;
        default:
                ret = -LINUX_EINVAL;
                break;
        }

        ref_put(&reply->refcount, free_message_port_ref);
        req->result = ret;
        return ret;
}

i64 vfs_backend_ipc_register(const char *port_name, const char *fstype,
                             u32 caps, u32 reg_flags)
{
        Message_Port_t *reply;
        i64 ret;

        if (!port_name || !port_name[0]) {
                return -LINUX_EINVAL;
        }

        reply = vfs_backend_ipc_reply_port();
        if (!reply) {
                return -LINUX_ENOMEM;
        }

        ret = ipc_rpc_call_named(VFS_SERVER_PORT_NAME,
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
