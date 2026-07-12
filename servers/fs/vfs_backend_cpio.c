#include "vfs_backend.h"

#include "cpio_rofs.h"
#include "vfs_backend_ipc.h"
#include "vfs_page_cache.h"

#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/initcall.h>
#include <linux_compat/ipc/rpc.h>
#include <modules/log/log.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/task/tcb.h>

static u16 vfs_cpio_service_id;
static Thread_Base *vfs_cpio_thread_ptr;
static bool vfs_cpio_server_done;

static void vfs_inode_fill_path(vfs_inode_t *ino, const char *path)
{
        if (!ino || !path) {
                return;
        }
        strncpy(ino->path, path, sizeof(ino->path) - 1);
        ino->path[sizeof(ino->path) - 1] = '\0';
}

static i64 vfs_backend_cpio_lookup(vfs_backend_req_t *req)
{
        cpio_rofs_stat_t st;

        if (!req || !req->path || !req->ino_out) {
                return -LINUX_EINVAL;
        }
        if (!cpio_rofs_lookup(req->path, &st)) {
                return -LINUX_ENOENT;
        }

        memset(req->ino_out, 0, sizeof(*req->ino_out));
        vfs_inode_fill_path(req->ino_out, req->path);
        req->ino_out->backend_port = VFS_BACKEND_PORT_CPIO;
        req->ino_out->backend_caps =
                vfs_backend_caps_for_port(VFS_BACKEND_PORT_CPIO);
        req->ino_out->storage = (void *)st.data;
        req->ino_out->mode = st.mode;
        req->ino_out->size = st.size;
        req->ino_out->nlink = st.nlink ? st.nlink : 1u;
        req->ino_out->is_dir = st.is_dir;
        req->ino_out->writable = false;
        return 0;
}

static i64 vfs_backend_cpio_read(vfs_backend_req_t *req)
{
        if (!req || !req->ino || !req->buf) {
                return -LINUX_EINVAL;
        }
        if (req->ino->is_dir) {
                return -LINUX_EISDIR;
        }

        return vfs_page_cache_read_inode(req->ino, req->offset, req->buf,
                                         req->len);
}

static i64 vfs_backend_cpio_write(vfs_backend_req_t *req)
{
        if (!req || !req->ino || !req->wbuf) {
                return -LINUX_EINVAL;
        }
        if (req->ino->is_dir) {
                return -LINUX_EISDIR;
        }

        return vfs_page_cache_write_inode(req->ino, req->offset, req->wbuf,
                                          req->len);
}

static i64 vfs_backend_cpio_flush(vfs_backend_req_t *req)
{
        if (!req || !req->ino) {
                return -LINUX_EINVAL;
        }

        vfs_page_cache_drop_backing(req->ino);
        return 0;
}

static i64 vfs_backend_cpio_service(vfs_backend_req_t *req)
{
        if (!req) {
                return -LINUX_EINVAL;
        }

        switch (req->op) {
        case VFS_BACKEND_OP_LOOKUP:
                return vfs_backend_cpio_lookup(req);
        case VFS_BACKEND_OP_READ:
                return vfs_backend_cpio_read(req);
        case VFS_BACKEND_OP_WRITE:
                return vfs_backend_cpio_write(req);
        case VFS_BACKEND_OP_TRUNCATE:
                return -LINUX_EROFS;
        case VFS_BACKEND_OP_FLUSH:
                return vfs_backend_cpio_flush(req);
        default:
                return -LINUX_EINVAL;
        }
}

static i64 vfs_cpio_rpc_handler(u16 opcode, const kmsg_t *km,
                                char **reply_port_out)
{
        return vfs_backend_ipc_rpc_handler(opcode, km, reply_port_out,
                                           vfs_backend_cpio_service);
}

static void vfs_cpio_thread_entry(void)
{
        i64 reg_ret;

        reg_ret = vfs_backend_ipc_register(
                VFS_BACKEND_PORT_CPIO,
                VFS_BACKEND_FSTYPE_CPIO,
                VFS_BACKEND_CAP_READ_SOURCE | VFS_BACKEND_CAP_WRITE_CACHE
                        | VFS_BACKEND_CAP_FLUSH_DROP,
                VFS_BACKEND_REG_ROOT);
        if (reg_ret < 0) {
                pr_error("[VFS/cpio] register with server failed: %lld\n",
                         (long long)reg_ret);
                return;
        }
        vfs_backend_mark_online(VFS_BACKEND_REG_ROOT);

        ipc_rpc_server_loop(VFS_BACKEND_PORT_CPIO,
                            vfs_cpio_service_id,
                            IPC_RPC_RESP_OPCODE_DEFAULT,
                            IPC_RPC_RESP_FMT_DEFAULT,
                            vfs_cpio_rpc_handler);
}

static void vfs_backend_cpio_init(void)
{
        error_t err;

        if (!linux_init_vfs_service_once(&vfs_cpio_server_done)) {
                return;
        }

        err = vfs_backend_ipc_server_spawn(VFS_BACKEND_PORT_CPIO,
                                           "vfs_cpio_backend_thread",
                                           &vfs_cpio_service_id,
                                           &vfs_cpio_thread_ptr,
                                           vfs_cpio_thread_entry);
        if (err != REND_SUCCESS) {
                pr_error("[VFS/cpio] server spawn failed: %d on CPU %llu\n",
                         (int)err,
                         (u64)percpu(cpu_number));
                return;
        }

        pr_info("[VFS/cpio] backend thread on CPU %llu port '%s'\n",
                (u64)percpu(cpu_number),
                VFS_BACKEND_PORT_CPIO);
        linux_init_vfs_service_mark_done(&vfs_cpio_server_done);
}

DEFINE_INIT_LEVEL(vfs_backend_cpio_init, 5);
