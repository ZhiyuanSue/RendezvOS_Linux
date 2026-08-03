#ifndef _VFS_RPC_H_
#define _VFS_RPC_H_

#include <common/stdbool.h>
#include <common/types.h>
#include <rendezvos/task/id.h>
#include <rendezvos/task/tcb.h>

bool vfs_rpc_client_pid(const char *reply_port_name, pid_t *pid_out);

/* User task with a user vspace, or NULL. */
Tcb_Base *vfs_task_user_for_pid(pid_t pid);

#endif /* _VFS_RPC_H_ */
