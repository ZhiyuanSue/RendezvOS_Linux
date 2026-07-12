#include "vfs_perm.h"

#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <rendezvos/task/tcb.h>

static vfs_req_cred_t vfs_req_cred;

void vfs_perm_set_request(u32 uid, u32 gid)
{
        vfs_req_cred.uid = uid;
        vfs_req_cred.gid = gid;
}

void vfs_perm_get_request(u32 *uid_out, u32 *gid_out)
{
        if (uid_out) {
                *uid_out = vfs_req_cred.uid;
        }
        if (gid_out) {
                *gid_out = vfs_req_cred.gid;
        }
}

bool vfs_perm_task_cred(pid_t pid, u32 *uid_out, u32 *gid_out)
{
        Tcb_Base *task = find_task_by_pid(pid);
        linux_proc_append_t *pa;

        if (!task) {
                return false;
        }

        pa = linux_proc_append(task);
        if (!pa) {
                return false;
        }

        if (uid_out) {
                *uid_out = pa->euid;
        }
        if (gid_out) {
                *gid_out = pa->egid;
        }
        return true;
}

i64 vfs_perm_check_mode(u32 mode, u32 mask, u32 uid, u32 gid)
{
        u32 perm;

        (void)gid;

        if (uid == 0) {
                return 0;
        }

        perm = (mode >> 6) & 7u;
        if (perm == 0) {
                perm = 7u;
        }

        if ((mask & VFS_PERM_R) && !(perm & 4u)) {
                return -LINUX_EACCES;
        }
        if ((mask & VFS_PERM_W) && !(perm & 2u)) {
                return -LINUX_EACCES;
        }
        if ((mask & VFS_PERM_X) && !(perm & 1u)) {
                return -LINUX_EACCES;
        }

        return 0;
}

i64 vfs_perm_check_mode_request(u32 mode, u32 mask)
{
        return vfs_perm_check_mode(mode, mask, vfs_req_cred.uid, vfs_req_cred.gid);
}
