/*
 * Nested FSM for OPEN / LOOKUP-like / MKDIR / UNLINK / RENAME / LINK / GETDENTS.
 */

#include "vfs_coop_internal.h"

#include "vfs_backend.h"
#include "vfs_handle.h"
#include "vfs_kstat.h"
#include "vfs_namespace.h"
#include "vfs_open.h"
#include "vfs_perm.h"
#include "vfs_rpc.h"
#include "vfs_mount.h"

#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/linux_mm_radix.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/vmm.h>

#define VFS_S_IFREG 0100000u

static void vfs_coop_set_port(vfs_coop_ctx_t *ctx, const char *port)
{
        if (!ctx || !port) {
                return;
        }
        strncpy(ctx->port, port, sizeof(ctx->port) - 1);
        ctx->port[sizeof(ctx->port) - 1] = '\0';
}

static void vfs_coop_adopt_path2(vfs_coop_ctx_t *ctx)
{
        if (!ctx) {
                return;
        }
        strncpy(ctx->path, ctx->path2, sizeof(ctx->path) - 1);
        ctx->path[sizeof(ctx->path) - 1] = '\0';
}

static error_t vfs_coop_park(ipc_rpc_coop_job_t *job, vfs_coop_ctx_t *ctx,
                             vfs_backend_op_t op, vfs_coop_step_t step)
{
        vfs_backend_req_t req;

        memset(&req, 0, sizeof(req));
        req.port = ctx->port;
        req.op = op;
        req.mode_arg = ctx->mode;
        switch (op) {
        case VFS_BACKEND_OP_LOOKUP:
                req.path = ctx->path2[0] ? ctx->path2 : ctx->path;
                req.ino_out = &ctx->ino;
                break;
        case VFS_BACKEND_OP_RENAME:
        case VFS_BACKEND_OP_LINK:
                req.path = ctx->path;
                req.path2 = ctx->path2;
                break;
        case VFS_BACKEND_OP_READDIR:
                req.path = ctx->path2[0] ? ctx->path2 : ctx->path;
                req.dir_index = ctx->dir_index;
                req.dirent_out = &ctx->dent;
                break;
        case VFS_BACKEND_OP_TRUNCATE:
                req.path = ctx->path;
                req.ino = &ctx->ino;
                req.size_arg = 0;
                break;
        case VFS_BACKEND_OP_READLINK:
                req.path = ctx->path;
                req.readlink_buf = ctx->path2;
                req.readlink_cap = sizeof(ctx->path2);
                break;
        default:
                req.path = ctx->path;
                break;
        }
        ctx->step = step;
        vfs_coop_running = ctx;
        return vfs_coop_issue_nested(job, &req);
}

static ipc_rpc_coop_disp_t vfs_coop_park_or_eio(ipc_rpc_coop_job_t *job,
                                                vfs_coop_ctx_t *ctx,
                                                vfs_backend_op_t op,
                                                vfs_coop_step_t step)
{
        error_t e = vfs_coop_park(job, ctx, op, step);

        if (e == REND_SUCCESS || e == -E_REND_AGAIN) {
                return IPC_RPC_COOP_PARKED;
        }
        vfs_coop_finish(job, -LINUX_EIO);
        return IPC_RPC_COOP_HANDLED;
}

typedef i64 (*vfs_coop_twopath_prep_fn)(const char *oldpath, const char *newpath,
                                        const char **port_out, char *old_out,
                                        u64 old_cap, char *new_out, u64 new_cap,
                                        bool *need_commit);

/* prepare() normalizes into locals first — overlapping in/out is safe. */
static ipc_rpc_coop_disp_t
vfs_coop_start_twopath(ipc_rpc_coop_job_t *job, vfs_coop_ctx_t *ctx,
                       vfs_coop_twopath_prep_fn prepare, vfs_backend_op_t op,
                       vfs_coop_step_t step)
{
        const char *port = NULL;
        bool need_commit = false;
        i64 prep;

        prep = prepare(ctx->path,
                       ctx->path2,
                       &port,
                       ctx->path,
                       sizeof(ctx->path),
                       ctx->path2,
                       sizeof(ctx->path2),
                       &need_commit);
        if (prep < 0) {
                vfs_coop_finish(job, prep);
                return IPC_RPC_COOP_HANDLED;
        }
        ctx->need_commit = need_commit;
        vfs_coop_set_port(ctx, port);
        return vfs_coop_park_or_eio(job, ctx, op, step);
}

static void vfs_coop_finish_after_be(ipc_rpc_coop_job_t *job,
                                     vfs_coop_ctx_t *ctx, i64 nested_result,
                                     i64 (*commit)(vfs_coop_ctx_t *))
{
        if (nested_result < 0) {
                vfs_coop_finish(job, nested_result);
                return;
        }
        if (ctx->need_commit) {
                vfs_coop_finish(job, commit(ctx));
                return;
        }
        vfs_coop_finish(job, 0);
}

static i64 vfs_coop_commit_unlink(vfs_coop_ctx_t *ctx)
{
        return vfs_namespace_unlink_commit(ctx->path);
}

static i64 vfs_coop_commit_rename(vfs_coop_ctx_t *ctx)
{
        return vfs_namespace_rename_commit(ctx->path, ctx->path2);
}

static i64 vfs_coop_commit_link(vfs_coop_ctx_t *ctx)
{
        return vfs_namespace_link_commit(ctx->path, ctx->path2);
}

/* 0 = followed (path updated); 1 = no follow; <0 errno. */
static i64 vfs_coop_try_follow_symlink(vfs_coop_ctx_t *ctx)
{
        char target[VFS_PATH_MAX];
        char resolved[VFS_PATH_MAX];
        const char *base;

        if (!ctx->follow_symlink || !ctx->ino.is_symlink) {
                return 1;
        }
        if (!vfs_inode_symlink_target(&ctx->ino, target, sizeof(target))) {
                return -LINUX_EINVAL;
        }
        base = ctx->path2[0] ? ctx->path2 : ctx->path;
        vfs_join_symlink_target(base, target, resolved, sizeof(resolved));
        if (resolved[0] == '\0') {
                return -LINUX_EINVAL;
        }
        strncpy(ctx->path, resolved, sizeof(ctx->path) - 1);
        ctx->path[sizeof(ctx->path) - 1] = '\0';
        ctx->path2[0] = '\0';
        ctx->follow_symlink = false;
        return 0;
}

static i64 vfs_coop_finish_lookup_op(vfs_coop_ctx_t *ctx)
{
        u32 check = 0;
        linux_proc_resource_t *task;

        switch (ctx->kind) {
        case VFS_COOP_KIND_STAT:
                task = vfs_task_user_for_pid(ctx->pid);
                if (!task) {
                        return -LINUX_ESRCH;
                }
                return vfs_store_inode_stat(task, ctx->user_buf, &ctx->ino);
        case VFS_COOP_KIND_CHDIR:
                if (!ctx->ino.is_dir) {
                        return -LINUX_ENOTDIR;
                }
                return 0;
        case VFS_COOP_KIND_FACCESSAT:
                if (ctx->mode == 0) {
                        return 0;
                }
                if (ctx->mode & 4u) {
                        check |= VFS_PERM_R;
                }
                if (ctx->mode & 2u) {
                        check |= VFS_PERM_W;
                }
                if (ctx->mode & 1u) {
                        check |= VFS_PERM_X;
                }
                return vfs_perm_check_mode_request(ctx->ino.mode, check);
        default:
                return -LINUX_EINVAL;
        }
}

static i64 vfs_coop_store_readlink(vfs_coop_ctx_t *ctx, const char *buf,
                                   i64 len)
{
        linux_proc_resource_t *task;
        u64 copy_len;
        error_t e;

        if (len < 0) {
                return len;
        }
        task = vfs_task_user_for_pid(ctx->pid);
        if (!task) {
                return -LINUX_ESRCH;
        }
        copy_len = (u64)len;
        if (copy_len >= ctx->count) {
                copy_len = ctx->count - 1;
        }
        e = linux_mm_store_to_user(
                task->vs, ctx->user_buf, buf, (size_t)copy_len + 1);
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        return len;
}

static ipc_rpc_coop_disp_t vfs_coop_after_readlink_inode(ipc_rpc_coop_job_t *job,
                                                         vfs_coop_ctx_t *ctx)
{
        vfs_mount_view_t mount_view;
        const char *port = NULL;
        i64 ret;

        if (!ctx->ino.is_symlink) {
                vfs_coop_finish(job, -LINUX_EINVAL);
                return IPC_RPC_COOP_HANDLED;
        }

        if (vfs_inode_symlink_target(&ctx->ino, ctx->path2, sizeof(ctx->path2))) {
                ret = vfs_coop_store_readlink(
                        ctx, ctx->path2, (i64)strlen(ctx->path2));
                vfs_coop_finish(job, ret);
                return IPC_RPC_COOP_HANDLED;
        }

        if (vfs_mount_view_for_path(ctx->path, &mount_view)) {
                port = mount_view.backend_port;
        } else if (ctx->ino.backend_port) {
                port = ctx->ino.backend_port;
        }
        if (!port) {
                vfs_coop_finish(job, -LINUX_EINVAL);
                return IPC_RPC_COOP_HANDLED;
        }

        vfs_coop_set_port(ctx, port);
        ctx->path2[0] = '\0';
        return vfs_coop_park_or_eio(
                job, ctx, VFS_BACKEND_OP_READLINK, VFS_COOP_STEP_READLINK);
}

static ipc_rpc_coop_disp_t vfs_coop_after_inode(ipc_rpc_coop_job_t *job,
                                                vfs_coop_ctx_t *ctx)
{
        i32 acc;
        i64 ret;

        if (ctx->kind == VFS_COOP_KIND_READLINK) {
                return vfs_coop_after_readlink_inode(job, ctx);
        }

        if (ctx->kind != VFS_COOP_KIND_OPEN) {
                vfs_coop_finish(job, vfs_coop_finish_lookup_op(ctx));
                return IPC_RPC_COOP_HANDLED;
        }

        if ((ctx->flags & (VFS_O_CREAT | VFS_O_EXCL))
            == (VFS_O_CREAT | VFS_O_EXCL)) {
                vfs_coop_finish(job, -LINUX_EEXIST);
                return IPC_RPC_COOP_HANDLED;
        }

        acc = ctx->flags & VFS_O_ACCMODE;
        if ((ctx->flags & VFS_O_TRUNC) && !ctx->ino.is_dir
            && (acc == VFS_O_WRONLY || acc == VFS_O_RDWR)) {
                if (!ctx->ino.writable) {
                        vfs_coop_finish(job, -LINUX_EROFS);
                        return IPC_RPC_COOP_HANDLED;
                }
                if (!ctx->ino.backend_port) {
                        vfs_coop_finish(job, -LINUX_ENXIO);
                        return IPC_RPC_COOP_HANDLED;
                }
                vfs_coop_set_port(ctx, ctx->ino.backend_port);
                return vfs_coop_park_or_eio(
                        job, ctx, VFS_BACKEND_OP_TRUNCATE, VFS_COOP_STEP_TRUNC);
        }

        ret = vfs_open_install(&ctx->ino, ctx->flags);
        vfs_coop_finish(job, ret);
        return IPC_RPC_COOP_HANDLED;
}

static ipc_rpc_coop_disp_t vfs_coop_open_on_enoent(ipc_rpc_coop_job_t *job,
                                                   vfs_coop_ctx_t *ctx)
{
        const char *port = NULL;
        u32 be_mode = 0;
        bool need_commit = false;
        i64 prep;

        if (!(ctx->flags & VFS_O_CREAT)) {
                vfs_coop_finish(job, -LINUX_ENOENT);
                return IPC_RPC_COOP_HANDLED;
        }

        if (ctx->flags & VFS_O_DIRECTORY) {
                prep = vfs_namespace_mkdir_prepare(ctx->path,
                                                   ctx->mode,
                                                   &port,
                                                   ctx->path2,
                                                   sizeof(ctx->path2),
                                                   &be_mode,
                                                   &need_commit);
                if (prep < 0) {
                        vfs_coop_finish(job, prep);
                        return IPC_RPC_COOP_HANDLED;
                }
                ctx->mode = be_mode;
                ctx->need_commit = need_commit;
                vfs_coop_set_port(ctx, port);
                vfs_coop_adopt_path2(ctx);
                return vfs_coop_park_or_eio(
                        job, ctx, VFS_BACKEND_OP_MKDIR, VFS_COOP_STEP_MKDIR_BE);
        }

        be_mode = (ctx->mode & 0777u) | VFS_S_IFREG;
        prep = vfs_namespace_create_prepare(ctx->path,
                                            be_mode,
                                            &ctx->ino,
                                            &port,
                                            ctx->path2,
                                            sizeof(ctx->path2),
                                            &be_mode,
                                            &need_commit);
        if (prep < 0) {
                vfs_coop_finish(job, prep);
                return IPC_RPC_COOP_HANDLED;
        }
        ctx->mode = be_mode;
        vfs_coop_set_port(ctx, port);
        vfs_coop_adopt_path2(ctx);
        ctx->path2[0] = '\0';
        if (prep == 2) {
                ctx->need_commit = false;
                return vfs_coop_park_or_eio(job,
                                            ctx,
                                            VFS_BACKEND_OP_LOOKUP,
                                            VFS_COOP_STEP_CREATE_EXISTING);
        }
        ctx->need_commit = need_commit;
        return vfs_coop_park_or_eio(
                job, ctx, VFS_BACKEND_OP_CREATE, VFS_COOP_STEP_CREATE);
}

static ipc_rpc_coop_disp_t vfs_coop_start_lookup(ipc_rpc_coop_job_t *job,
                                                 vfs_coop_ctx_t *ctx)
{
        const char *port = NULL;
        i64 prep;
        i64 follow;

        ctx->path2[0] = '\0';
        prep = vfs_namespace_lookup_prepare(
                ctx->path, &ctx->ino, &port, ctx->path2, sizeof(ctx->path2));
        if (prep < 0) {
                if (ctx->kind == VFS_COOP_KIND_OPEN && prep == -LINUX_ENOENT) {
                        return vfs_coop_open_on_enoent(job, ctx);
                }
                vfs_coop_finish(job, prep);
                return IPC_RPC_COOP_HANDLED;
        }
        if (prep == 0) {
                follow = vfs_coop_try_follow_symlink(ctx);
                if (follow < 0) {
                        vfs_coop_finish(job, follow);
                        return IPC_RPC_COOP_HANDLED;
                }
                if (follow == 0) {
                        return vfs_coop_start_lookup(job, ctx);
                }
                return vfs_coop_after_inode(job, ctx);
        }

        vfs_coop_set_port(ctx, port);
        return vfs_coop_park_or_eio(
                job, ctx, VFS_BACKEND_OP_LOOKUP, VFS_COOP_STEP_LOOKUP);
}

ipc_rpc_coop_disp_t vfs_coop_start_path(ipc_rpc_coop_job_t *job,
                                        vfs_coop_ctx_t *ctx)
{
        const char *port = NULL;
        u32 be_mode = 0;
        bool need_commit = false;
        bool need_backend = false;
        i64 prep;

        if (!job || !ctx) {
                return IPC_RPC_COOP_HANDLED;
        }

        vfs_coop_apply_cred(ctx);

        if (ctx->kind == VFS_COOP_KIND_READLINK && ctx->count == 0) {
                vfs_coop_finish(job, -LINUX_EINVAL);
                return IPC_RPC_COOP_HANDLED;
        }

        if (ctx->kind == VFS_COOP_KIND_MKDIR) {
                prep = vfs_namespace_mkdir_prepare(ctx->path,
                                                   ctx->mode,
                                                   &port,
                                                   ctx->path2,
                                                   sizeof(ctx->path2),
                                                   &be_mode,
                                                   &need_commit);
                if (prep < 0) {
                        vfs_coop_finish(job, prep);
                        return IPC_RPC_COOP_HANDLED;
                }
                ctx->mode = be_mode;
                ctx->need_commit = need_commit;
                vfs_coop_set_port(ctx, port);
                vfs_coop_adopt_path2(ctx);
                return vfs_coop_park_or_eio(
                        job, ctx, VFS_BACKEND_OP_MKDIR, VFS_COOP_STEP_MKDIR_BE);
        }

        if (ctx->kind == VFS_COOP_KIND_UNLINK) {
                if (ctx->flags & 0x200) {
                        vfs_coop_finish(job, -LINUX_ENOSYS);
                        return IPC_RPC_COOP_HANDLED;
                }
                prep = vfs_namespace_unlink_prepare(ctx->path,
                                                    &port,
                                                    ctx->path2,
                                                    sizeof(ctx->path2),
                                                    &need_backend,
                                                    &need_commit);
                if (prep < 0) {
                        vfs_coop_finish(job, prep);
                        return IPC_RPC_COOP_HANDLED;
                }
                vfs_coop_adopt_path2(ctx);
                ctx->need_commit = need_commit;
                if (!need_backend) {
                        vfs_coop_finish(job,
                                        need_commit ?
                                                vfs_namespace_unlink_commit(
                                                        ctx->path) :
                                                0);
                        return IPC_RPC_COOP_HANDLED;
                }
                vfs_coop_set_port(ctx, port);
                return vfs_coop_park_or_eio(job,
                                            ctx,
                                            VFS_BACKEND_OP_UNLINK,
                                            VFS_COOP_STEP_UNLINK_BE);
        }

        if (ctx->kind == VFS_COOP_KIND_RENAME) {
                return vfs_coop_start_twopath(job,
                                               ctx,
                                               vfs_namespace_rename_prepare,
                                               VFS_BACKEND_OP_RENAME,
                                               VFS_COOP_STEP_RENAME_BE);
        }

        if (ctx->kind == VFS_COOP_KIND_LINK) {
                return vfs_coop_start_twopath(job,
                                               ctx,
                                               vfs_namespace_link_prepare,
                                               VFS_BACKEND_OP_LINK,
                                               VFS_COOP_STEP_LINK_BE);
        }

        if (ctx->kind == VFS_COOP_KIND_MOUNT) {
                bool need_mkdir = false;

                prep = vfs_mount_register_prepare(ctx->path,
                                                  ctx->path2,
                                                  ctx->count,
                                                  &port,
                                                  ctx->path,
                                                  sizeof(ctx->path),
                                                  &need_mkdir);
                if (prep < 0) {
                        vfs_coop_finish(job, prep);
                        return IPC_RPC_COOP_HANDLED;
                }
                if (prep == 0) {
                        vfs_coop_finish(job, 0);
                        return IPC_RPC_COOP_HANDLED;
                }
                if (!need_mkdir) {
                        (void)vfs_mount_apply_cover(ctx->path, true);
                        vfs_coop_finish(job, 0);
                        return IPC_RPC_COOP_HANDLED;
                }
                ctx->mode = 0755u | 0040000u;
                vfs_coop_set_port(ctx, port);
                return vfs_coop_park_or_eio(
                        job, ctx, VFS_BACKEND_OP_MKDIR, VFS_COOP_STEP_MKDIR_BE);
        }

        return vfs_coop_start_lookup(job, ctx);
}

void vfs_coop_resume_path(ipc_rpc_coop_job_t *job, vfs_coop_ctx_t *ctx,
                          i64 nested_result)
{
        i64 ret;
        i64 follow;

        if (!job || !ctx) {
                return;
        }

        vfs_coop_apply_cred(ctx);
        vfs_coop_running = ctx;

        switch (ctx->step) {
        case VFS_COOP_STEP_LOOKUP:
                if (nested_result != 0) {
                        if (ctx->kind == VFS_COOP_KIND_OPEN
                            && nested_result == -LINUX_ENOENT) {
                                (void)vfs_coop_open_on_enoent(job, ctx);
                                return;
                        }
                        vfs_coop_finish(job, nested_result);
                        return;
                }
                follow = vfs_coop_try_follow_symlink(ctx);
                if (follow < 0) {
                        vfs_coop_finish(job, follow);
                        return;
                }
                if (follow == 0) {
                        (void)vfs_coop_start_lookup(job, ctx);
                        return;
                }
                (void)vfs_coop_after_inode(job, ctx);
                return;

        case VFS_COOP_STEP_CREATE_EXISTING:
                if (nested_result != 0) {
                        vfs_coop_finish(job, nested_result);
                        return;
                }
                if (!ctx->ino.writable) {
                        vfs_coop_finish(job, -LINUX_EEXIST);
                        return;
                }
                (void)vfs_coop_after_inode(job, ctx);
                return;

        case VFS_COOP_STEP_CREATE:
                if (nested_result < 0) {
                        vfs_coop_finish(job, nested_result);
                        return;
                }
                if (ctx->need_commit) {
                        ret = vfs_namespace_create_commit(
                                ctx->path, ctx->mode, NULL);
                        if (ret < 0) {
                                vfs_coop_finish(job, ret);
                                return;
                        }
                }
                ctx->path2[0] = '\0';
                if (vfs_coop_park_or_eio(job,
                                         ctx,
                                         VFS_BACKEND_OP_LOOKUP,
                                         VFS_COOP_STEP_POST_CREATE_LOOKUP)
                    == IPC_RPC_COOP_PARKED) {
                        return;
                }
                return;

        case VFS_COOP_STEP_POST_CREATE_LOOKUP:
                if (nested_result != 0) {
                        vfs_coop_finish(job, -LINUX_EIO);
                        return;
                }
                (void)vfs_coop_after_inode(job, ctx);
                return;

        case VFS_COOP_STEP_MKDIR_BE:
                if (ctx->kind == VFS_COOP_KIND_MOUNT) {
                        /* Best-effort mkdir (same as former sync mount). */
                        (void)nested_result;
                        (void)vfs_mount_apply_cover(ctx->path, true);
                        vfs_coop_finish(job, 0);
                        return;
                }
                if (nested_result < 0) {
                        vfs_coop_finish(job, nested_result);
                        return;
                }
                if (ctx->need_commit) {
                        ret = vfs_namespace_mkdir_commit(ctx->path, ctx->mode);
                        if (ret < 0) {
                                vfs_coop_finish(job, ret);
                                return;
                        }
                }
                if (ctx->kind == VFS_COOP_KIND_OPEN) {
                        ctx->path2[0] = '\0';
                        ctx->follow_symlink = false;
                        (void)vfs_coop_start_lookup(job, ctx);
                        return;
                }
                vfs_coop_finish(job, 0);
                return;

        case VFS_COOP_STEP_TRUNC:
                if (nested_result < 0) {
                        vfs_coop_finish(job, nested_result);
                        return;
                }
                ctx->ino.size = 0;
                ret = vfs_open_install(&ctx->ino, ctx->flags);
                vfs_coop_finish(job, ret);
                return;

        case VFS_COOP_STEP_UNLINK_BE:
                vfs_coop_finish_after_be(
                        job, ctx, nested_result, vfs_coop_commit_unlink);
                return;

        case VFS_COOP_STEP_RENAME_BE:
                vfs_coop_finish_after_be(
                        job, ctx, nested_result, vfs_coop_commit_rename);
                return;

        case VFS_COOP_STEP_LINK_BE:
                vfs_coop_finish_after_be(
                        job, ctx, nested_result, vfs_coop_commit_link);
                return;

        case VFS_COOP_STEP_READLINK:
                if (nested_result < 0) {
                        vfs_coop_finish(job, nested_result);
                        return;
                }
                vfs_coop_finish(
                        job,
                        vfs_coop_store_readlink(
                                ctx, ctx->path2, nested_result));
                return;

        default:
                vfs_coop_finish(job, -LINUX_EIO);
                return;
        }
}

#define VFS_DIRENT64_HDR 19u

static u16 vfs_dirent64_reclen(u64 name_len)
{
        u16 reclen = (u16)(VFS_DIRENT64_HDR + name_len + 1);

        return (u16)((reclen + 7u) & ~7u);
}

/* Bytes packed, 0 = no room (stop), <0 errno. */
static i64 vfs_coop_getdents_pack_one(vfs_coop_ctx_t *ctx)
{
        linux_proc_resource_t *task;
        u64 name_len;
        u16 reclen;
        u64 next_index;
        u8 chunk[512];
        error_t e;

        task = vfs_task_user_for_pid(ctx->pid);
        if (!task) {
                return -LINUX_ESRCH;
        }

        name_len = strlen(ctx->dent.name);
        reclen = vfs_dirent64_reclen(name_len);
        if (ctx->total + reclen > ctx->count) {
                return 0;
        }
        if (reclen > sizeof(chunk)) {
                return -LINUX_EINVAL;
        }

        memset(chunk, 0, reclen);
        memcpy(chunk, &ctx->dent.d_ino, sizeof(ctx->dent.d_ino));
        next_index = ctx->dir_index + 1;
        memcpy(chunk + 8, &next_index, sizeof(next_index));
        memcpy(chunk + 16, &reclen, sizeof(reclen));
        chunk[18] = ctx->dent.d_type;
        memcpy(chunk + VFS_DIRENT64_HDR, ctx->dent.name, name_len + 1);

        e = linux_mm_store_to_user(
                task->vs, ctx->user_buf + ctx->total, chunk, reclen);
        if (e != REND_SUCCESS) {
                return ctx->total > 0 ? 0 : -LINUX_EFAULT;
        }
        return (i64)reclen;
}

static void vfs_coop_getdents_commit_offset(vfs_coop_ctx_t *ctx)
{
        vfs_open_handle_t *file = vfs_handle_get(ctx->handle);

        if (file) {
                file->offset = ctx->dir_index;
        }
}

static ipc_rpc_coop_disp_t vfs_coop_getdents_fill(ipc_rpc_coop_job_t *job,
                                                   vfs_coop_ctx_t *ctx)
{
        const char *port = NULL;
        i64 prep;
        i64 packed;

        while (ctx->total < ctx->count) {
                ctx->path2[0] = '\0';
                prep = vfs_namespace_readdir_prepare(ctx->path,
                                                     ctx->dir_index,
                                                     &ctx->dent,
                                                     &port,
                                                     ctx->path2,
                                                     sizeof(ctx->path2));
                if (prep < 0) {
                        vfs_coop_getdents_commit_offset(ctx);
                        vfs_coop_finish(job, prep);
                        return IPC_RPC_COOP_HANDLED;
                }
                if (prep == 1) {
                        vfs_coop_getdents_commit_offset(ctx);
                        vfs_coop_finish(job, (i64)ctx->total);
                        return IPC_RPC_COOP_HANDLED;
                }
                if (prep == 2) {
                        vfs_coop_set_port(ctx, port);
                        return vfs_coop_park_or_eio(job,
                                                    ctx,
                                                    VFS_BACKEND_OP_READDIR,
                                                    VFS_COOP_STEP_READDIR);
                }

                packed = vfs_coop_getdents_pack_one(ctx);
                if (packed < 0) {
                        vfs_coop_getdents_commit_offset(ctx);
                        vfs_coop_finish(job, packed);
                        return IPC_RPC_COOP_HANDLED;
                }
                if (packed == 0) {
                        if (ctx->total == 0) {
                                vfs_coop_finish(job, -LINUX_EINVAL);
                        } else {
                                vfs_coop_getdents_commit_offset(ctx);
                                vfs_coop_finish(job, (i64)ctx->total);
                        }
                        return IPC_RPC_COOP_HANDLED;
                }
                ctx->total += (u64)packed;
                ctx->dir_index++;
        }

        vfs_coop_getdents_commit_offset(ctx);
        vfs_coop_finish(job, (i64)ctx->total);
        return IPC_RPC_COOP_HANDLED;
}

ipc_rpc_coop_disp_t vfs_coop_start_getdents(ipc_rpc_coop_job_t *job,
                                            vfs_coop_ctx_t *ctx)
{
        vfs_open_handle_t *file;

        if (!job || !ctx) {
                return IPC_RPC_COOP_HANDLED;
        }

        vfs_coop_apply_cred(ctx);

        if (ctx->count == 0) {
                vfs_coop_finish(job, 0);
                return IPC_RPC_COOP_HANDLED;
        }

        file = vfs_handle_get(ctx->handle);
        if (!file) {
                vfs_coop_finish(job, -LINUX_EBADF);
                return IPC_RPC_COOP_HANDLED;
        }
        if (!file->ino.is_dir) {
                vfs_coop_finish(job, -LINUX_ENOTDIR);
                return IPC_RPC_COOP_HANDLED;
        }

        strncpy(ctx->path, file->ino.path, sizeof(ctx->path) - 1);
        ctx->path[sizeof(ctx->path) - 1] = '\0';
        ctx->dir_index = file->offset;
        ctx->total = 0;
        return vfs_coop_getdents_fill(job, ctx);
}

void vfs_coop_resume_getdents(ipc_rpc_coop_job_t *job, vfs_coop_ctx_t *ctx,
                              i64 nested_result)
{
        i64 packed;

        if (!job || !ctx) {
                return;
        }

        vfs_coop_apply_cred(ctx);
        vfs_coop_running = ctx;

        if (ctx->step != VFS_COOP_STEP_READDIR) {
                vfs_coop_finish(job, -LINUX_EIO);
                return;
        }

        if (nested_result < 0) {
                vfs_coop_getdents_commit_offset(ctx);
                vfs_coop_finish(job, nested_result);
                return;
        }
        if (nested_result > 0) {
                /* EOF from backend */
                vfs_coop_getdents_commit_offset(ctx);
                vfs_coop_finish(job, (i64)ctx->total);
                return;
        }

        packed = vfs_coop_getdents_pack_one(ctx);
        if (packed < 0) {
                vfs_coop_getdents_commit_offset(ctx);
                vfs_coop_finish(job, packed);
                return;
        }
        if (packed == 0) {
                if (ctx->total == 0) {
                        vfs_coop_finish(job, -LINUX_EINVAL);
                } else {
                        vfs_coop_getdents_commit_offset(ctx);
                        vfs_coop_finish(job, (i64)ctx->total);
                }
                return;
        }
        ctx->total += (u64)packed;
        ctx->dir_index++;
        (void)vfs_coop_getdents_fill(job, ctx);
}
