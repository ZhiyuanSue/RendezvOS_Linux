#ifndef _LINUX_COMPAT_FS_LINUX_PIPE_H_
#define _LINUX_COMPAT_FS_LINUX_PIPE_H_

#include <common/types.h>
#include <linux_compat/proc_compat.h>

i64 linux_pipe_create2(linux_proc_resource_t *task, u64 user_pipefd, i32 flags);
/**
 * @brief After fork/dup: one more open end of this pipe.
 * @param read_end true if the new fd is the read end.
 */
void linux_pipe_fork_retain(u32 pipe_id, bool read_end);
void linux_pipe_fd_closed(u32 pipe_id, bool read_end);
i64 linux_pipe_read(linux_proc_resource_t *task, u32 pipe_id, u64 user_buf, u64 count);
i64 linux_pipe_write(linux_proc_resource_t *task, u32 pipe_id, u64 user_buf, u64 count);

#endif /* _LINUX_COMPAT_FS_LINUX_PIPE_H_ */
