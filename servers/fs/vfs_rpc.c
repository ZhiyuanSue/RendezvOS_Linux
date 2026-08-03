#include "vfs_rpc.h"

#include <common/string.h>
#include <linux_compat/ipc/port_naming.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_registry.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/task/tcb.h>

bool vfs_rpc_client_pid(const char *reply_port_name, pid_t *pid_out)
{
        const char *pfx = VFS_CLIENT_PORT_PREFIX;
        u64 pfx_len;
        u64 i;
        pid_t pid = 0;

        if (!reply_port_name || !pid_out) {
                return false;
        }

        pfx_len = strlen(pfx);
        for (i = 0; i < pfx_len; i++) {
                if (reply_port_name[i] != pfx[i]) {
                        return false;
                }
        }

        for (i = pfx_len; reply_port_name[i] != '\0'; i++) {
                char c = reply_port_name[i];

                if (c < '0' || c > '9') {
                        return false;
                }
                pid = pid * 10 + (pid_t)(c - '0');
        }

        if (pid <= 0) {
                return false;
        }

        *pid_out = pid;
        return true;
}

Tcb_Base *vfs_task_user_for_pid(pid_t pid)
{
        Tcb_Base *task = find_task_by_pid(pid);

        if (!task || !task->vs || !linux_vspace_is_user_table(task->vs)) {
                return NULL;
        }
        return task;
}
