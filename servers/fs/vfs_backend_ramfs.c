#include "vfs_backend.h"

#include "ramfs_layer.h"
#include "vfs_backend_ipc.h"
#include "vfs_page_cache.h"

#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/initcall.h>
#include <linux_compat/ipc/rpc.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>

static u16 vfs_ramfs_service_id;
static Thread_Base *vfs_ramfs_thread_ptr;
static bool vfs_ramfs_server_done;

static void vfs_inode_fill_path(vfs_inode_t *ino, const char *path)
{
        if (!ino || !path) {
                return;
        }
        strncpy(ino->path, path, sizeof(ino->path) - 1);
        ino->path[sizeof(ino->path) - 1] = '\0';
}

static i64 vfs_backend_ramfs_lookup(vfs_backend_req_t *req)
{
        const ramfs_entry_t *ent;

        if (!req || !req->path || !req->ino_out) {
                return -LINUX_EINVAL;
        }

        ent = ramfs_lookup(req->path);
        if (!ent) {
                return -LINUX_ENOENT;
        }

        memset(req->ino_out, 0, sizeof(*req->ino_out));
        vfs_inode_fill_path(req->ino_out, req->path);
        req->ino_out->backend_port = VFS_BACKEND_PORT_RAMFS;
        req->ino_out->backend_caps =
                vfs_backend_caps_for_port(VFS_BACKEND_PORT_RAMFS);
        req->ino_out->storage = (void *)ent;
        req->ino_out->mode = ent->mode;
        req->ino_out->size = ent->size;
        req->ino_out->nlink = 1;
        req->ino_out->is_dir = (ent->flags & RAMFS_FLAG_DIR) != 0;
        req->ino_out->writable = true;
        return 0;
}

static i64 vfs_backend_ramfs_read(vfs_backend_req_t *req)
{
        if (!req || !req->ino || !req->buf || !req->ino->storage) {
                return -LINUX_EINVAL;
        }
        if (req->ino->is_dir) {
                return -LINUX_EISDIR;
        }

        return vfs_page_cache_read_inode(req->ino, req->offset, req->buf,
                                         req->len);
}

static i64 vfs_backend_ramfs_write(vfs_backend_req_t *req)
{
        ramfs_entry_t *ent;
        i64 ret;
        u64 old_size;

        if (!req || !req->ino || !req->wbuf || !req->ino->storage) {
                return -LINUX_EINVAL;
        }
        if (req->ino->is_dir) {
                return -LINUX_EISDIR;
        }

        ent = (ramfs_entry_t *)req->ino->storage;
        old_size = req->ino->size;
        ret = ramfs_write(ent, req->offset, req->wbuf, req->len);
        if (ret < 0) {
                if (ret == (i64)-E_IN_PARAM) {
                        return -LINUX_EINVAL;
                }
                return -LINUX_ENOMEM;
        }

        req->ino->size = ent->size;
        if (req->ino->size > old_size) {
                vfs_page_cache_drop(req->ino->path);
        } else if (ret > 0) {
                vfs_page_cache_sync_write(req->ino, req->offset, req->wbuf,
                                          (u64)ret);
        }
        return ret;
}

static i64 vfs_backend_ramfs_truncate(vfs_backend_req_t *req)
{
        ramfs_entry_t *ent;
        error_t err;

        if (!req || !req->ino || !req->ino->storage) {
                return -LINUX_EINVAL;
        }
        if (req->ino->is_dir) {
                return -LINUX_EISDIR;
        }

        ent = (ramfs_entry_t *)req->ino->storage;
        err = ramfs_truncate(ent, req->size_arg);
        if (err != REND_SUCCESS) {
                return -LINUX_ENOMEM;
        }

        vfs_page_cache_drop(req->ino->path);
        req->ino->size = ent->size;
        return 0;
}

static i64 vfs_backend_ramfs_flush(vfs_backend_req_t *req)
{
        if (!req || !req->ino) {
                return -LINUX_EINVAL;
        }

        return vfs_page_cache_flush_backing(req->ino);
}

static i64 vfs_backend_ramfs_service(vfs_backend_req_t *req)
{
        if (!req) {
                return -LINUX_EINVAL;
        }

        switch (req->op) {
        case VFS_BACKEND_OP_LOOKUP:
                return vfs_backend_ramfs_lookup(req);
        case VFS_BACKEND_OP_READ:
                return vfs_backend_ramfs_read(req);
        case VFS_BACKEND_OP_WRITE:
                return vfs_backend_ramfs_write(req);
        case VFS_BACKEND_OP_TRUNCATE:
                return vfs_backend_ramfs_truncate(req);
        case VFS_BACKEND_OP_FLUSH:
                return vfs_backend_ramfs_flush(req);
        default:
                return -LINUX_EINVAL;
        }
}

static i64 vfs_ramfs_rpc_handler(u16 opcode, const kmsg_t *km,
                                 char **reply_port_out)
{
        return vfs_backend_ipc_rpc_handler(opcode, km, reply_port_out,
                                           vfs_backend_ramfs_service);
}

static void vfs_ramfs_thread_entry(void)
{
        i64 reg_ret;

        reg_ret = vfs_backend_ipc_register(
                VFS_BACKEND_PORT_RAMFS,
                VFS_BACKEND_FSTYPE_RAMFS,
                VFS_BACKEND_CAP_READ_SOURCE | VFS_BACKEND_CAP_WRITE_SOURCE
                        | VFS_BACKEND_CAP_WRITE_CACHE,
                VFS_BACKEND_REG_OVERLAY);
        if (reg_ret < 0) {
                pr_error("[VFS/ramfs] register with server failed: %lld\n",
                         (long long)reg_ret);
                return;
        }
        vfs_backend_mark_online(VFS_BACKEND_REG_OVERLAY);

        ipc_rpc_server_loop(VFS_BACKEND_PORT_RAMFS,
                            vfs_ramfs_service_id,
                            IPC_RPC_RESP_OPCODE_DEFAULT,
                            IPC_RPC_RESP_FMT_DEFAULT,
                            vfs_ramfs_rpc_handler);
}

static void vfs_backend_ramfs_init(void)
{
        error_t err;

        if (!linux_init_vfs_service_once(&vfs_ramfs_server_done)) {
                return;
        }

        err = vfs_backend_ipc_server_spawn(VFS_BACKEND_PORT_RAMFS,
                                           "vfs_ramfs_backend_thread",
                                           &vfs_ramfs_service_id,
                                           &vfs_ramfs_thread_ptr,
                                           vfs_ramfs_thread_entry);
        if (err != REND_SUCCESS) {
                pr_error("[VFS/ramfs] server spawn failed: %d on CPU %llu\n",
                         (int)err,
                         (u64)percpu(cpu_number));
                return;
        }

        pr_info("[VFS/ramfs] backend thread on CPU %llu port '%s'\n",
                (u64)percpu(cpu_number),
                VFS_BACKEND_PORT_RAMFS);
        linux_init_vfs_service_mark_done(&vfs_ramfs_server_done);
}

DEFINE_INIT_LEVEL(vfs_backend_ramfs_init, 5);
