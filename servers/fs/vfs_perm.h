#ifndef _VFS_PERM_H_
#define _VFS_PERM_H_

#include <common/stdbool.h>
#include <common/types.h>
#include <rendezvos/task/id.h>

#define VFS_PERM_R 4u
#define VFS_PERM_W 2u
#define VFS_PERM_X 1u

typedef struct vfs_req_cred {
        u32 uid;
        u32 gid;
} vfs_req_cred_t;

void vfs_perm_set_request(u32 uid, u32 gid);
void vfs_perm_get_request(u32 *uid_out, u32 *gid_out);

bool vfs_perm_task_cred(pid_t pid, u32 *uid_out, u32 *gid_out);

/* uid 0 bypasses; others use owner mode bits. */
i64 vfs_perm_check_mode(u32 mode, u32 mask, u32 uid, u32 gid);

i64 vfs_perm_check_mode_request(u32 mode, u32 mask);

#endif /* _VFS_PERM_H_ */
