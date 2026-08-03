#include "vfs_backend.h"

#include "vfs_backend_ipc.h"

#include <common/mm.h>
#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/initcall.h>
#include <linux_compat/ipc/rpc.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/page_slice.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/task/tcb.h>

#define VFS_BLKDEV_PSEUDO_SIZE (64u * 1024u)

static struct page_slice *vfs_blkdev_slice;
static u16 vfs_blkdev_service_id;
static Thread_Base *vfs_blkdev_thread_ptr;
static bool vfs_blkdev_server_done;

static error_t vfs_blkdev_ensure_slice(void)
{
        struct allocator *alloc;
        u64 pgoff;
        u64 pages;
        error_t err;

        if (vfs_blkdev_slice) {
                return REND_SUCCESS;
        }

        alloc = percpu(kallocator);
        if (!alloc || !alloc->m_alloc) {
                return -E_REND_NO_MEM;
        }

        vfs_blkdev_slice = page_slice_create(0, VFS_BLKDEV_PSEUDO_SIZE);
        if (!vfs_blkdev_slice) {
                return -E_REND_NO_MEM;
        }

        pages = PAGE_SLICE_SIZE_TO_PAGE_COUNT(VFS_BLKDEV_PSEUDO_SIZE);
        for (pgoff = 0; pgoff < pages; pgoff++) {
                vaddr page = (vaddr)alloc->m_alloc(alloc, PAGE_SIZE);

                if (!page) {
                        page_slice_destroy(&vfs_blkdev_slice);
                        return -E_REND_NO_MEM;
                }
                memset((void *)page, 0, PAGE_SIZE);
                err = page_slice_insert_page(vfs_blkdev_slice, pgoff, page, 0);
                if (err != REND_SUCCESS) {
                        alloc->m_free(alloc, (void *)page);
                        page_slice_destroy(&vfs_blkdev_slice);
                        return err;
                }
        }
        return REND_SUCCESS;
}

static i64 vfs_blkdev_rw(u64 offset, void *buf, u64 len, bool is_write)
{
        u64 done = 0;

        if (!buf) {
                return -LINUX_EINVAL;
        }
        if (vfs_blkdev_ensure_slice() != REND_SUCCESS) {
                return -LINUX_ENOMEM;
        }
        if (offset >= VFS_BLKDEV_PSEUDO_SIZE) {
                return 0;
        }
        if (len > VFS_BLKDEV_PSEUDO_SIZE - offset) {
                len = VFS_BLKDEV_PSEUDO_SIZE - offset;
        }

        while (done < len) {
                u64 off = offset + done;
                u64 pgoff = PAGE_SLICE_BYTE_TO_PGOFF(off);
                u64 in_page = PAGE_SLICE_IN_PAGE_OFF(off);
                struct page_slice_entry *entry;
                size_t chunk;

                entry = page_slice_lookup(vfs_blkdev_slice, pgoff);
                if (!entry) {
                        return -LINUX_EIO;
                }
                chunk = PAGE_SIZE - (size_t)in_page;
                if (chunk > len - done) {
                        chunk = (size_t)(len - done);
                }
                if (is_write) {
                        memcpy((void *)(entry->kernel_virtual_address
                                        + in_page),
                               (const u8 *)buf + done,
                               chunk);
                } else {
                        memcpy((u8 *)buf + done,
                               (void *)(entry->kernel_virtual_address
                                        + in_page),
                               chunk);
                }
                done += chunk;
        }
        return (i64)done;
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
                return vfs_blkdev_rw(req->offset, req->buf, req->len, false);
        case VFS_BACKEND_OP_WRITE:
                return vfs_blkdev_rw(
                        req->offset, (void *)req->wbuf, req->len, true);
        case VFS_BACKEND_OP_TRUNCATE:
                return -LINUX_ENOSYS;
        case VFS_BACKEND_OP_FLUSH:
                return 0;
        default:
                return -LINUX_EINVAL;
        }
}

static void vfs_blkdev_thread_entry(void)
{
        static ipc_rpc_coop_queue_t vfs_blkdev_coop_q;
        i64 reg_ret;

        if (vfs_blkdev_ensure_slice() != REND_SUCCESS) {
                pr_error("[VFS/blkdev] slice alloc failed\n");
                return;
        }

        reg_ret = vfs_backend_ipc_leaf_run(
                VFS_BACKEND_PORT_BLKDEV,
                VFS_BACKEND_FSTYPE_BLKDEV,
                VFS_BACKEND_CAP_READ_SOURCE | VFS_BACKEND_CAP_WRITE_SOURCE,
                0,
                vfs_blkdev_service_id,
                &vfs_blkdev_coop_q,
                vfs_backend_blkdev_service);
        if (reg_ret < 0) {
                pr_error("[VFS/blkdev] register with server failed: %lld\n",
                         (long long)reg_ret);
        }
}

static void vfs_backend_blkdev_init(void)
{
        (void)vfs_backend_ipc_leaf_spawn(VFS_BACKEND_PORT_BLKDEV,
                                         "vfs_blkdev_thread",
                                         "blkdev",
                                         &vfs_blkdev_service_id,
                                         &vfs_blkdev_thread_ptr,
                                         vfs_blkdev_thread_entry,
                                         &vfs_blkdev_server_done);
}

DEFINE_INIT_LEVEL(vfs_backend_blkdev_init, 5);
