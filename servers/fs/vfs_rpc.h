#ifndef _VFS_RPC_H_
#define _VFS_RPC_H_

#include <common/stdbool.h>
#include <common/types.h>
#include <rendezvos/task/id.h>

#include <linux_compat/fs/vfs_protocol.h>

bool vfs_rpc_client_pid(const char *reply_port_name, pid_t *pid_out);

#endif /* _VFS_RPC_H_ */
