#ifndef _VFS_SERVER_H_
#define _VFS_SERVER_H_

#include <common/types.h>
#include <rendezvos/task/tcb.h>

#include <linux_compat/fs/vfs_protocol.h>

/* VFS listen kthread — nest replies transfer here (coop NESTED_RECV drain). */
Thread_Base *vfs_server_thread_get(void);

void vfs_server_thread(void);

#endif /* _VFS_SERVER_H_ */
