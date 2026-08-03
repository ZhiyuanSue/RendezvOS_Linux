#ifndef _VFS_COOP_INTERNAL_H_
#define _VFS_COOP_INTERNAL_H_

#include "vfs_backend.h"
#include "vfs_backend_ops.h"
#include "vfs_kstat.h"

#include <linux_compat/fs/vfs_path.h>
#include <linux_compat/ipc/port_naming.h>
#include <linux_compat/ipc/rpc.h>
#include <rendezvos/task/tcb.h>

typedef enum {
        VFS_COOP_KIND_NONE = 0,
        VFS_COOP_KIND_READ,
        VFS_COOP_KIND_WRITE,
        VFS_COOP_KIND_OPEN,
        VFS_COOP_KIND_MKDIR,
        VFS_COOP_KIND_UNLINK,
        VFS_COOP_KIND_STAT,
        VFS_COOP_KIND_CHDIR,
        VFS_COOP_KIND_FACCESSAT,
        VFS_COOP_KIND_RENAME,
        VFS_COOP_KIND_LINK,
        VFS_COOP_KIND_GETDENTS,
        VFS_COOP_KIND_READLINK,
        VFS_COOP_KIND_MOUNT,
} vfs_coop_kind_t;

typedef enum {
        VFS_COOP_STEP_NONE = 0,
        VFS_COOP_STEP_LOOKUP,
        VFS_COOP_STEP_CREATE,
        VFS_COOP_STEP_CREATE_EXISTING,
        VFS_COOP_STEP_POST_CREATE_LOOKUP,
        VFS_COOP_STEP_MKDIR_BE,
        VFS_COOP_STEP_TRUNC,
        VFS_COOP_STEP_UNLINK_BE,
        VFS_COOP_STEP_RENAME_BE,
        VFS_COOP_STEP_LINK_BE,
        VFS_COOP_STEP_READDIR,
        VFS_COOP_STEP_READLINK,
} vfs_coop_step_t;

typedef struct vfs_coop_ctx {
        vfs_coop_kind_t kind;
        vfs_coop_step_t step;
        pid_t pid;
        u32 uid;
        u32 gid;
        /* READ/WRITE / GETDENTS */
        u32 handle;
        u64 user_buf;
        u64 count;
        u64 total;
        u64 remaining;
        u64 chunk_len;
        u8 *chunk;
        u64 dir_index;
        vfs_dirent_t dent;
        /* Path ops */
        char path[VFS_PATH_MAX];
        char path2[VFS_PATH_MAX];
        char port[PORT_NAME_LEN_MAX];
        i32 flags;
        u32 mode;
        u32 at_flags;
        bool follow_symlink;
        bool need_commit;
        vfs_inode_t ino;
} vfs_coop_ctx_t;

extern vfs_coop_ctx_t *vfs_coop_running;

void vfs_coop_finish(ipc_rpc_coop_job_t *job, i64 result);
void vfs_coop_apply_cred(const vfs_coop_ctx_t *ctx);
error_t vfs_coop_issue_nested(ipc_rpc_coop_job_t *job, vfs_backend_req_t *req);
vfs_coop_ctx_t *vfs_coop_ctx_alloc(void);
bool vfs_coop_fill_cred(vfs_coop_ctx_t *ctx, pid_t pid);

ipc_rpc_coop_disp_t vfs_coop_start_path(ipc_rpc_coop_job_t *job,
                                        vfs_coop_ctx_t *ctx);
void vfs_coop_resume_path(ipc_rpc_coop_job_t *job, vfs_coop_ctx_t *ctx,
                          i64 nested_result);

ipc_rpc_coop_disp_t vfs_coop_start_getdents(ipc_rpc_coop_job_t *job,
                                            vfs_coop_ctx_t *ctx);
void vfs_coop_resume_getdents(ipc_rpc_coop_job_t *job, vfs_coop_ctx_t *ctx,
                              i64 nested_result);

#endif /* _VFS_COOP_INTERNAL_H_ */
