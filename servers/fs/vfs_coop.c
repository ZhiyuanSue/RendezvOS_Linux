/*
 * VFS listen coop: per-job cred + I/O scratch; nested park for path/I/O.
 * Local one-shot ops (close/lseek/fstat/umount/register) finish inline.
 */

#include "vfs_coop.h"
#include "vfs_coop_internal.h"

#include "vfs_backend.h"
#include "vfs_backend_ipc.h"
#include "vfs_handle.h"
#include "vfs_mount.h"
#include "vfs_open.h"
#include "vfs_perm.h"
#include "vfs_rpc.h"

#include <common/mm.h>
#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_registry.h>
#include <rendezvos/error.h>
#include <rendezvos/ipc/ipc_serial.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>

#if PAGE_SIZE < 4096u
#define VFS_COOP_IO_CHUNK PAGE_SIZE
#else
#define VFS_COOP_IO_CHUNK 4096u
#endif

vfs_coop_ctx_t *vfs_coop_running;

void vfs_coop_apply_cred(const vfs_coop_ctx_t *ctx)
{
        if (!ctx) {
                vfs_perm_set_request(0, 0);
                return;
        }
        vfs_perm_set_request(ctx->uid, ctx->gid);
}

static void vfs_coop_ctx_free(vfs_coop_ctx_t *ctx)
{
        struct allocator *alloc = percpu(kallocator);

        if (!ctx) {
                return;
        }
        if (ctx->chunk && alloc && alloc->m_free) {
                alloc->m_free(alloc, ctx->chunk);
                ctx->chunk = NULL;
        }
        if (alloc && alloc->m_free) {
                alloc->m_free(alloc, ctx);
        }
}

void vfs_coop_finish(ipc_rpc_coop_job_t *job, i64 result)
{
        vfs_coop_ctx_t *ctx;

        if (!job) {
                return;
        }
        ctx = (vfs_coop_ctx_t *)job->cookie;
        job->cookie = NULL;
        if (vfs_coop_running == ctx) {
                vfs_coop_running = NULL;
        }
        vfs_coop_ctx_free(ctx);
        ipc_rpc_coop_job_set_result(job, result);
}

error_t vfs_coop_issue_nested(ipc_rpc_coop_job_t *job, vfs_backend_req_t *req)
{
        error_t e;
        int tries;

        if (!job || !req) {
                return -E_IN_PARAM;
        }

        for (tries = 0; tries < 64; tries++) {
                e = vfs_backend_ipc_coop_nested(job, req);
                if (e == REND_SUCCESS) {
                        return REND_SUCCESS;
                }
                if (e == -E_REND_AGAIN
                    && (job->state == IPC_RPC_COOP_ST_NESTED_SEND
                        || job->state == IPC_RPC_COOP_ST_NESTED_RECV)) {
                        return -E_REND_AGAIN;
                }
                if (e != -E_REND_AGAIN) {
                        return e;
                }
                schedule(percpu(core_tm));
        }
        return -E_RENDEZVOS;
}

static error_t vfs_coop_issue_rw_nested(ipc_rpc_coop_job_t *job,
                                        vfs_coop_ctx_t *ctx,
                                        vfs_open_handle_t *file)
{
        vfs_backend_req_t req;

        memset(&req, 0, sizeof(req));
        req.port = file->ino.backend_port;
        req.ino = &file->ino;
        req.offset = file->offset;
        req.len = ctx->chunk_len;

        if (ctx->kind == VFS_COOP_KIND_READ) {
                req.op = VFS_BACKEND_OP_READ;
                req.buf = ctx->chunk;
        } else {
                req.op = VFS_BACKEND_OP_WRITE;
                req.wbuf = ctx->chunk;
        }

        return vfs_coop_issue_nested(job, &req);
}

static ipc_rpc_coop_disp_t vfs_coop_start_rw(ipc_rpc_coop_job_t *job,
                                             vfs_coop_ctx_t *ctx)
{
        vfs_open_handle_t *file;
        Tcb_Base *task;
        error_t e;
        struct allocator *alloc = percpu(kallocator);

        task = vfs_task_user_for_pid(ctx->pid);
        if (!task) {
                vfs_coop_finish(job, -LINUX_ESRCH);
                return IPC_RPC_COOP_HANDLED;
        }

        file = vfs_handle_get(ctx->handle);
        if (!file) {
                vfs_coop_finish(job, -LINUX_EBADF);
                return IPC_RPC_COOP_HANDLED;
        }

        if (file->ino.is_dir) {
                vfs_coop_finish(job, -LINUX_EISDIR);
                return IPC_RPC_COOP_HANDLED;
        }

        if (ctx->kind == VFS_COOP_KIND_READ) {
                if ((file->open_flags & VFS_O_ACCMODE) == VFS_O_WRONLY) {
                        vfs_coop_finish(job, -LINUX_EBADF);
                        return IPC_RPC_COOP_HANDLED;
                }
        } else {
                if ((file->open_flags & VFS_O_ACCMODE) == VFS_O_RDONLY) {
                        vfs_coop_finish(job, -LINUX_EBADF);
                        return IPC_RPC_COOP_HANDLED;
                }
                if (file->open_flags & VFS_O_APPEND) {
                        file->offset = file->ino.size;
                }
        }

        if (!file->ino.backend_port) {
                vfs_coop_finish(job, 0);
                return IPC_RPC_COOP_HANDLED;
        }

        if (!alloc || !alloc->m_alloc) {
                vfs_coop_finish(job, -LINUX_ENOMEM);
                return IPC_RPC_COOP_HANDLED;
        }

        ctx->chunk = (u8 *)alloc->m_alloc(alloc, PAGE_SIZE);
        if (!ctx->chunk) {
                vfs_coop_finish(job, -LINUX_ENOMEM);
                return IPC_RPC_COOP_HANDLED;
        }

        ctx->remaining = ctx->count;
        ctx->total = 0;
        if (ctx->remaining == 0) {
                vfs_coop_finish(job, 0);
                return IPC_RPC_COOP_HANDLED;
        }

        ctx->chunk_len = ctx->remaining;
        if (ctx->chunk_len > VFS_COOP_IO_CHUNK) {
                ctx->chunk_len = VFS_COOP_IO_CHUNK;
        }

        if (ctx->kind == VFS_COOP_KIND_WRITE) {
                if (linux_mm_load_from_user(task->vs,
                                            ctx->user_buf,
                                            ctx->chunk,
                                            (size_t)ctx->chunk_len)
                    != REND_SUCCESS) {
                        vfs_coop_finish(job, -LINUX_EFAULT);
                        return IPC_RPC_COOP_HANDLED;
                }
        }

        vfs_coop_running = ctx;
        e = vfs_coop_issue_rw_nested(job, ctx, file);
        if (e == REND_SUCCESS || e == -E_REND_AGAIN) {
                return IPC_RPC_COOP_PARKED;
        }
        vfs_coop_running = NULL;
        vfs_coop_finish(job, -LINUX_EIO);
        return IPC_RPC_COOP_HANDLED;
}

static void vfs_coop_resume_rw(ipc_rpc_coop_job_t *job, vfs_coop_ctx_t *ctx,
                               i64 nested_result)
{
        vfs_open_handle_t *file;
        Tcb_Base *task;
        error_t e;
        i64 n = nested_result;

        vfs_coop_apply_cred(ctx);
        vfs_coop_running = ctx;

        task = vfs_task_user_for_pid(ctx->pid);
        file = vfs_handle_get(ctx->handle);
        if (!task || !file) {
                vfs_coop_finish(job,
                                ctx->total > 0 ? (i64)ctx->total :
                                                 -LINUX_EBADF);
                return;
        }

        if (n < 0) {
                vfs_coop_finish(job, ctx->total > 0 ? (i64)ctx->total : n);
                return;
        }
        if (n == 0) {
                vfs_coop_finish(job, (i64)ctx->total);
                return;
        }

        if (ctx->kind == VFS_COOP_KIND_READ) {
                if (linux_mm_store_to_user(task->vs,
                                           ctx->user_buf + ctx->total,
                                           ctx->chunk,
                                           (size_t)n)
                    != REND_SUCCESS) {
                        vfs_coop_finish(job,
                                        ctx->total > 0 ? (i64)ctx->total :
                                                         -LINUX_EFAULT);
                        return;
                }
        }

        file->offset += (u64)n;
        ctx->total += (u64)n;
        if (ctx->remaining >= (u64)n) {
                ctx->remaining -= (u64)n;
        } else {
                ctx->remaining = 0;
        }

        if (ctx->remaining == 0 || (u64)n < ctx->chunk_len) {
                vfs_coop_finish(job, (i64)ctx->total);
                return;
        }

        ctx->chunk_len = ctx->remaining;
        if (ctx->chunk_len > VFS_COOP_IO_CHUNK) {
                ctx->chunk_len = VFS_COOP_IO_CHUNK;
        }

        if (ctx->kind == VFS_COOP_KIND_WRITE) {
                if (linux_mm_load_from_user(task->vs,
                                            ctx->user_buf + ctx->total,
                                            ctx->chunk,
                                            (size_t)ctx->chunk_len)
                    != REND_SUCCESS) {
                        vfs_coop_finish(job, (i64)ctx->total);
                        return;
                }
        }

        e = vfs_coop_issue_rw_nested(job, ctx, file);
        if (e == REND_SUCCESS || e == -E_REND_AGAIN) {
                return;
        }
        vfs_coop_finish(job, ctx->total > 0 ? (i64)ctx->total : -LINUX_EIO);
}

static void vfs_coop_resume(ipc_rpc_coop_job_t *job, i64 nested_result,
                            void *opaque)
{
        vfs_coop_ctx_t *ctx;

        (void)opaque;
        if (!job) {
                return;
        }
        ctx = (vfs_coop_ctx_t *)job->cookie;
        if (!ctx) {
                ipc_rpc_coop_job_set_result(job, nested_result);
                return;
        }

        if (ctx->kind == VFS_COOP_KIND_READ || ctx->kind == VFS_COOP_KIND_WRITE) {
                vfs_coop_resume_rw(job, ctx, nested_result);
                return;
        }
        if (ctx->kind == VFS_COOP_KIND_GETDENTS) {
                vfs_coop_resume_getdents(job, ctx, nested_result);
                return;
        }

        vfs_coop_resume_path(job, ctx, nested_result);
}

void vfs_coop_queue_prepare(ipc_rpc_coop_queue_t *q)
{
        if (!q) {
                return;
        }
        if (!q->inited) {
                ipc_rpc_coop_queue_init(q);
        }
        ipc_rpc_coop_queue_set_resume(q, vfs_coop_resume, NULL);
}

vfs_coop_ctx_t *vfs_coop_ctx_alloc(void)
{
        struct allocator *alloc = percpu(kallocator);
        vfs_coop_ctx_t *ctx;

        if (!alloc || !alloc->m_alloc) {
                return NULL;
        }
        ctx = (vfs_coop_ctx_t *)alloc->m_alloc(alloc, sizeof(*ctx));
        if (!ctx) {
                return NULL;
        }
        memset(ctx, 0, sizeof(*ctx));
        return ctx;
}

bool vfs_coop_fill_cred(vfs_coop_ctx_t *ctx, pid_t pid)
{
        u32 uid = 0;
        u32 gid = 0;

        ctx->pid = pid;
        if (vfs_perm_task_cred(pid, &uid, &gid)) {
                ctx->uid = uid;
                ctx->gid = gid;
        } else {
                ctx->uid = 0;
                ctx->gid = 0;
        }
        vfs_coop_apply_cred(ctx);
        return true;
}

static bool vfs_coop_is_path_opcode(u16 opcode)
{
        switch (opcode) {
        case KMSG_OP_VFS_OPEN:
        case KMSG_OP_VFS_MKDIRAT:
        case KMSG_OP_VFS_UNLINKAT:
        case KMSG_OP_VFS_NEWFSTATAT:
        case KMSG_OP_VFS_CHDIR:
        case KMSG_OP_VFS_VALIDATE_DIR:
        case KMSG_OP_VFS_FACCESSAT:
        case KMSG_OP_VFS_RENAMEAT:
        case KMSG_OP_VFS_LINKAT:
        case KMSG_OP_VFS_READLINKAT:
        case KMSG_OP_VFS_MOUNT:
                return true;
        default:
                return false;
        }
}

static ipc_rpc_coop_disp_t vfs_coop_decode_path(ipc_rpc_coop_job_t *job,
                                                u16 opcode, const kmsg_t *km,
                                                i64 *result_out)
{
        error_t decode_err;
        u64 param1 = 0;
        u64 param2 = 0;
        char *str = NULL;
        char *str2 = NULL;
        char *rp = NULL;
        pid_t pid = 0;
        vfs_coop_ctx_t *ctx;

        decode_err = -E_IN_PARAM;
        switch (opcode) {
        case KMSG_OP_VFS_OPEN:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_OPEN "t",
                                               &str,
                                               &param1,
                                               &param2,
                                               &rp);
                break;
        case KMSG_OP_VFS_MKDIRAT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_MKDIRAT "t",
                                               &str,
                                               &param1,
                                               &rp);
                break;
        case KMSG_OP_VFS_UNLINKAT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_UNLINKAT "t",
                                               &str,
                                               &param1,
                                               &rp);
                break;
        case KMSG_OP_VFS_NEWFSTATAT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_NEWFSTATAT "t",
                                               &str,
                                               &param1,
                                               &param2,
                                               &rp);
                break;
        case KMSG_OP_VFS_CHDIR:
        case KMSG_OP_VFS_VALIDATE_DIR:
                decode_err = ipc_serial_decode(
                        km->payload,
                        km->hdr.payload_len,
                        (opcode == KMSG_OP_VFS_CHDIR) ?
                                VFS_KMSG_FMT_CHDIR "t" :
                                VFS_KMSG_FMT_VALIDATE_DIR "t",
                        &str,
                        &rp);
                break;
        case KMSG_OP_VFS_FACCESSAT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_FACCESSAT "t",
                                               &str,
                                               &param1,
                                               &param2,
                                               &rp);
                break;
        case KMSG_OP_VFS_RENAMEAT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_RENAMEAT "t",
                                               &str,
                                               &str2,
                                               &param1,
                                               &rp);
                break;
        case KMSG_OP_VFS_LINKAT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_LINKAT "t",
                                               &str,
                                               &str2,
                                               &param1,
                                               &rp);
                break;
        case KMSG_OP_VFS_READLINKAT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_READLINKAT "t",
                                               &str,
                                               &param1,
                                               &param2,
                                               &rp);
                break;
        case KMSG_OP_VFS_MOUNT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_MOUNT "t",
                                               &str,
                                               &str2,
                                               &param1,
                                               &rp);
                break;
        default:
                break;
        }

        if (decode_err != REND_SUCCESS || !str) {
                *result_out = -LINUX_EINVAL;
                return IPC_RPC_COOP_HANDLED;
        }
        if ((opcode == KMSG_OP_VFS_RENAMEAT || opcode == KMSG_OP_VFS_LINKAT
             || opcode == KMSG_OP_VFS_MOUNT)
            && !str2) {
                *result_out = -LINUX_EINVAL;
                return IPC_RPC_COOP_HANDLED;
        }

        if (!vfs_rpc_client_pid(job->reply_port, &pid)
            && !(rp && vfs_rpc_client_pid(rp, &pid))) {
                *result_out = -LINUX_EINVAL;
                return IPC_RPC_COOP_HANDLED;
        }

        ctx = vfs_coop_ctx_alloc();
        if (!ctx) {
                *result_out = -LINUX_ENOMEM;
                return IPC_RPC_COOP_HANDLED;
        }

        vfs_coop_fill_cred(ctx, pid);
        strncpy(ctx->path, str, sizeof(ctx->path) - 1);
        ctx->path[sizeof(ctx->path) - 1] = '\0';
        if (str2) {
                strncpy(ctx->path2, str2, sizeof(ctx->path2) - 1);
                ctx->path2[sizeof(ctx->path2) - 1] = '\0';
        }

        switch (opcode) {
        case KMSG_OP_VFS_OPEN:
                ctx->kind = VFS_COOP_KIND_OPEN;
                ctx->flags = (i32)param1;
                ctx->mode = (u32)param2;
                ctx->follow_symlink = true;
                break;
        case KMSG_OP_VFS_MKDIRAT:
                ctx->kind = VFS_COOP_KIND_MKDIR;
                ctx->mode = (u32)param1;
                break;
        case KMSG_OP_VFS_UNLINKAT:
                ctx->kind = VFS_COOP_KIND_UNLINK;
                ctx->flags = (i32)param1;
                break;
        case KMSG_OP_VFS_NEWFSTATAT:
                ctx->kind = VFS_COOP_KIND_STAT;
                ctx->user_buf = param1;
                ctx->at_flags = (u32)param2;
                ctx->follow_symlink =
                        (ctx->at_flags & VFS_AT_SYMLINK_NOFOLLOW) == 0;
                break;
        case KMSG_OP_VFS_CHDIR:
        case KMSG_OP_VFS_VALIDATE_DIR:
                ctx->kind = VFS_COOP_KIND_CHDIR;
                ctx->follow_symlink = true;
                break;
        case KMSG_OP_VFS_FACCESSAT:
                ctx->kind = VFS_COOP_KIND_FACCESSAT;
                ctx->mode = (u32)param1;
                ctx->at_flags = (u32)param2;
                ctx->follow_symlink =
                        (ctx->at_flags & VFS_AT_SYMLINK_NOFOLLOW) == 0;
                break;
        case KMSG_OP_VFS_RENAMEAT:
                ctx->kind = VFS_COOP_KIND_RENAME;
                ctx->flags = (i32)param1;
                break;
        case KMSG_OP_VFS_LINKAT:
                ctx->kind = VFS_COOP_KIND_LINK;
                ctx->flags = (i32)param1;
                break;
        case KMSG_OP_VFS_READLINKAT:
                ctx->kind = VFS_COOP_KIND_READLINK;
                ctx->user_buf = param1;
                ctx->count = param2;
                ctx->follow_symlink = false;
                break;
        case KMSG_OP_VFS_MOUNT:
                ctx->kind = VFS_COOP_KIND_MOUNT;
                ctx->count = param1; /* flags */
                break;
        default:
                vfs_coop_finish(job, -LINUX_ENOSYS);
                return IPC_RPC_COOP_HANDLED;
        }

        job->cookie = ctx;
        return vfs_coop_start_path(job, ctx);
}

ipc_rpc_coop_disp_t vfs_coop_handler(ipc_rpc_coop_job_t *job, u16 opcode,
                                     const kmsg_t *km, i64 *result_out)
{
        error_t decode_err;
        u64 param1 = 0;
        u64 param2 = 0;
        u64 param3 = 0;
        char *rp = NULL;
        pid_t pid = 0;
        vfs_coop_ctx_t *ctx;

        if (!job || !km || !result_out) {
                return IPC_RPC_COOP_HANDLED;
        }

        if (vfs_coop_is_path_opcode(opcode)) {
                return vfs_coop_decode_path(job, opcode, km, result_out);
        }

        if (opcode == KMSG_OP_VFS_GETDENTS64) {
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_GETDENTS64 "t",
                                               &param1,
                                               &param2,
                                               &param3,
                                               &rp);
                if (decode_err != REND_SUCCESS) {
                        *result_out = -LINUX_EINVAL;
                        return IPC_RPC_COOP_HANDLED;
                }
                if (!vfs_rpc_client_pid(job->reply_port, &pid)
                    && !(rp && vfs_rpc_client_pid(rp, &pid))) {
                        *result_out = -LINUX_EINVAL;
                        return IPC_RPC_COOP_HANDLED;
                }
                ctx = vfs_coop_ctx_alloc();
                if (!ctx) {
                        *result_out = -LINUX_ENOMEM;
                        return IPC_RPC_COOP_HANDLED;
                }
                vfs_coop_fill_cred(ctx, pid);
                ctx->kind = VFS_COOP_KIND_GETDENTS;
                ctx->handle = (u32)param1;
                ctx->user_buf = param2;
                ctx->count = param3;
                job->cookie = ctx;
                return vfs_coop_start_getdents(job, ctx);
        }

        if (opcode == KMSG_OP_VFS_READ || opcode == KMSG_OP_VFS_WRITE) {
                decode_err = ipc_serial_decode(
                        km->payload,
                        km->hdr.payload_len,
                        (opcode == KMSG_OP_VFS_READ) ? VFS_KMSG_FMT_READ "t" :
                                                       VFS_KMSG_FMT_WRITE "t",
                        &param1,
                        &param2,
                        &param3,
                        &rp);
                if (decode_err != REND_SUCCESS) {
                        *result_out = -LINUX_EINVAL;
                        return IPC_RPC_COOP_HANDLED;
                }

                if (!vfs_rpc_client_pid(job->reply_port, &pid)
                    && !(rp && vfs_rpc_client_pid(rp, &pid))) {
                        *result_out = -LINUX_EINVAL;
                        return IPC_RPC_COOP_HANDLED;
                }

                ctx = vfs_coop_ctx_alloc();
                if (!ctx) {
                        *result_out = -LINUX_ENOMEM;
                        return IPC_RPC_COOP_HANDLED;
                }

                vfs_coop_fill_cred(ctx, pid);
                ctx->kind = (opcode == KMSG_OP_VFS_READ) ? VFS_COOP_KIND_READ :
                                                           VFS_COOP_KIND_WRITE;
                ctx->handle = (u32)param1;
                ctx->user_buf = param2;
                ctx->count = param3;
                job->cookie = ctx;

                return vfs_coop_start_rw(job, ctx);
        }

        /* Local / one-shot listen ops (no nested backend park). */
        {
                char *str = NULL;
                char *str2 = NULL;
                u32 uid = 0;
                u32 gid = 0;

                decode_err = -E_IN_PARAM;
                switch (opcode) {
                case KMSG_OP_VFS_CLOSE:
                case KMSG_OP_VFS_HANDLE_RETAIN:
                        decode_err = ipc_serial_decode(
                                km->payload,
                                km->hdr.payload_len,
                                (opcode == KMSG_OP_VFS_CLOSE) ?
                                        VFS_KMSG_FMT_CLOSE "t" :
                                        VFS_KMSG_FMT_HANDLE_RETAIN "t",
                                &param1,
                                &rp);
                        break;
                case KMSG_OP_VFS_FSTAT:
                        decode_err = ipc_serial_decode(km->payload,
                                                       km->hdr.payload_len,
                                                       VFS_KMSG_FMT_FSTAT "t",
                                                       &param1,
                                                       &param2,
                                                       &rp);
                        break;
                case KMSG_OP_VFS_LSEEK:
                        decode_err = ipc_serial_decode(km->payload,
                                                       km->hdr.payload_len,
                                                       VFS_KMSG_FMT_LSEEK "t",
                                                       &param1,
                                                       &param2,
                                                       &param3,
                                                       &rp);
                        break;
                case KMSG_OP_VFS_UMOUNT:
                        decode_err = ipc_serial_decode(km->payload,
                                                       km->hdr.payload_len,
                                                       VFS_KMSG_FMT_UMOUNT "t",
                                                       &str,
                                                       &param1,
                                                       &rp);
                        break;
                case KMSG_OP_VFS_BACKEND_REGISTER:
                        decode_err = ipc_serial_decode(
                                km->payload,
                                km->hdr.payload_len,
                                VFS_KMSG_FMT_BACKEND_REGISTER "t",
                                &str,
                                &str2,
                                &param1,
                                &param2,
                                &rp);
                        break;
                case KMSG_OP_VFS_DUP3:
                        decode_err = ipc_serial_decode(km->payload,
                                                       km->hdr.payload_len,
                                                       VFS_KMSG_FMT_DUP3 "t",
                                                       &param1,
                                                       &param2,
                                                       &param3,
                                                       &rp);
                        *result_out = (decode_err == REND_SUCCESS) ?
                                              -LINUX_ENOSYS :
                                              -LINUX_EINVAL;
                        return IPC_RPC_COOP_HANDLED;
                case KMSG_OP_VFS_PIPE2:
                        decode_err = ipc_serial_decode(km->payload,
                                                       km->hdr.payload_len,
                                                       VFS_KMSG_FMT_PIPE2 "t",
                                                       &param1,
                                                       &param2,
                                                       &rp);
                        *result_out = (decode_err == REND_SUCCESS) ?
                                              -LINUX_ENOSYS :
                                              -LINUX_EINVAL;
                        return IPC_RPC_COOP_HANDLED;
                case KMSG_OP_VFS_GETCWD:
                        *result_out = -LINUX_ENOSYS;
                        return IPC_RPC_COOP_HANDLED;
                default:
                        decode_err = ipc_serial_decode(km->payload,
                                                       km->hdr.payload_len,
                                                       "t",
                                                       &rp);
                        *result_out = (decode_err == REND_SUCCESS) ?
                                              -LINUX_ENOSYS :
                                              -LINUX_EINVAL;
                        return IPC_RPC_COOP_HANDLED;
                }

                if (decode_err != REND_SUCCESS) {
                        *result_out = -LINUX_EINVAL;
                        return IPC_RPC_COOP_HANDLED;
                }

                if (opcode == KMSG_OP_VFS_BACKEND_REGISTER) {
                        *result_out = vfs_backend_register(
                                str, str2, (u32)param1, (u32)param2);
                        return IPC_RPC_COOP_HANDLED;
                }

                if (!vfs_rpc_client_pid(job->reply_port, &pid)
                    && !(rp && vfs_rpc_client_pid(rp, &pid))) {
                        *result_out = -LINUX_EINVAL;
                        return IPC_RPC_COOP_HANDLED;
                }

                if (vfs_perm_task_cred(pid, &uid, &gid)) {
                        vfs_perm_set_request(uid, gid);
                } else {
                        vfs_perm_set_request(0, 0);
                }

                switch (opcode) {
                case KMSG_OP_VFS_CLOSE:
                        *result_out = vfs_handle_close((u32)param1);
                        break;
                case KMSG_OP_VFS_HANDLE_RETAIN:
                        *result_out = vfs_handle_retain((u32)param1);
                        break;
                case KMSG_OP_VFS_FSTAT:
                        *result_out = vfs_fstat_handle(pid, (u32)param1, param2);
                        break;
                case KMSG_OP_VFS_LSEEK:
                        *result_out = vfs_lseek_handle(
                                (u32)param1, (i64)param2, (i32)param3);
                        break;
                case KMSG_OP_VFS_UMOUNT:
                        *result_out = vfs_mount_unregister(str, param1);
                        break;
                default:
                        *result_out = -LINUX_ENOSYS;
                        break;
                }
                return IPC_RPC_COOP_HANDLED;
        }
}
