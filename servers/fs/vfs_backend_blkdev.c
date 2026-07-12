#include "vfs_backend.h"

#include "vfs_backend_ipc.h"

#include <common/mm.h>
#include <linux_compat/errno.h>
#include <linux_compat/initcall.h>
#include <linux_compat/ipc/rpc.h>
#include <modules/log/log.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/task/tcb.h>

#define VFS_BLKDEV_PSEUDO_SIZE (64u * 1024u)

static u8 vfs_blkdev_mem[VFS_BLKDEV_PSEUDO_SIZE];
static u16 vfs_blkdev_service_id;
static Thread_Base *vfs_blkdev_thread_ptr;
static bool vfs_blkdev_server_done;

static i64 vfs_blkdev_read(u64 offset, void *buf, u64 len)
{
        if (!buf) {
                return -LINUX_EINVAL;
        }
        if (offset >= VFS_BLKDEV_PSEUDO_SIZE) {
                return 0;
        }
        if (len > VFS_BLKDEV_PSEUDO_SIZE - offset) {
                len = VFS_BLKDEV_PSEUDO_SIZE - offset;
        }
        memcpy(buf, vfs_blkdev_mem + offset, (size_t)len);
        return (i64)len;
}

static i64 vfs_blkdev_write(u64 offset, const void *buf, u64 len)
{
        if (!buf) {
                return -LINUX_EINVAL;
        }
        if (offset >= VFS_BLKDEV_PSEUDO_SIZE) {
                return 0;
        }
        if (len > VFS_BLKDEV_PSEUDO_SIZE - offset) {
                len = VFS_BLKDEV_PSEUDO_SIZE - offset;
        }
        memcpy(vfs_blkdev_mem + offset, buf, (size_t)len);
        return (i64)len;
}

static i64 vfs_backend_blkdev_service(vfs_backend_req_t *req)
{
        if (!req) {
                return -LINUX_EINVAL;
        }

        switch (req->op) {
        case VFS_BACKEND_OP_LOOKUP:
                return -LINUX_ENOSYS;
        case VFS_BACKEND_OP_READ:
                return vfs_blkdev_read(req->offset, req->buf, req->len);
        case VFS_BACKEND_OP_WRITE:
                return vfs_blkdev_write(req->offset, req->wbuf, req->len);
        case VFS_BACKEND_OP_TRUNCATE:
                return -LINUX_ENOSYS;
        case VFS_BACKEND_OP_FLUSH:
                return 0;
        default:
                return -LINUX_EINVAL;
        }
}

static i64 vfs_blkdev_rpc_handler(u16 opcode, const kmsg_t *km,
                                  char **reply_port_out)
{
        return vfs_backend_ipc_rpc_handler(opcode, km, reply_port_out,
                                           vfs_backend_blkdev_service);
}

static void vfs_blkdev_thread_entry(void)
{
        i64 reg_ret;

        reg_ret = vfs_backend_ipc_register(
                VFS_BACKEND_PORT_BLKDEV,
                VFS_BACKEND_FSTYPE_BLKDEV,
                VFS_BACKEND_CAP_READ_SOURCE | VFS_BACKEND_CAP_WRITE_SOURCE,
                0);
        if (reg_ret < 0) {
                pr_error("[VFS/blkdev] register with server failed: %lld\n",
                         (long long)reg_ret);
                return;
        }

        ipc_rpc_server_loop(VFS_BACKEND_PORT_BLKDEV,
                            vfs_blkdev_service_id,
                            IPC_RPC_RESP_OPCODE_DEFAULT,
                            IPC_RPC_RESP_FMT_DEFAULT,
                            vfs_blkdev_rpc_handler);
}

static void vfs_backend_blkdev_init(void)
{
        error_t err;

        if (!linux_init_vfs_service_once(&vfs_blkdev_server_done)) {
                return;
        }

        err = vfs_backend_ipc_server_spawn(VFS_BACKEND_PORT_BLKDEV,
                                           "vfs_blkdev_thread",
                                           &vfs_blkdev_service_id,
                                           &vfs_blkdev_thread_ptr,
                                           vfs_blkdev_thread_entry);
        if (err != REND_SUCCESS) {
                pr_error("[VFS/blkdev] server spawn failed: %d on CPU %llu\n",
                         (int)err,
                         (u64)percpu(cpu_number));
                return;
        }

        pr_info("[VFS/blkdev] backend thread on CPU %llu port '%s'\n",
                (u64)percpu(cpu_number),
                VFS_BACKEND_PORT_BLKDEV);
        linux_init_vfs_service_mark_done(&vfs_blkdev_server_done);
}

DEFINE_INIT_LEVEL(vfs_backend_blkdev_init, 5);
