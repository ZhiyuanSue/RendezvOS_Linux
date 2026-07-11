#ifndef _LINUX_COMPAT_FS_LINUX_PIPE_H_
#define _LINUX_COMPAT_FS_LINUX_PIPE_H_

#include <common/types.h>
#include <rendezvos/task/tcb.h>

i64 linux_pipe_create2(Tcb_Base *task, u64 user_pipefd, i32 flags);
void linux_pipe_fork_retain(u32 pipe_id);
void linux_pipe_fd_closed(u32 pipe_id, bool read_end);
i64 linux_pipe_read(Tcb_Base *task, u32 pipe_id, u64 user_buf, u64 count);
i64 linux_pipe_write(Tcb_Base *task, u32 pipe_id, u64 user_buf, u64 count);

#endif /* _LINUX_COMPAT_FS_LINUX_PIPE_H_ */
