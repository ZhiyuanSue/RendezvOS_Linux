#ifndef _RENDEZVOS_LINUX_SYSCALL_H_
#define _RENDEZVOS_LINUX_SYSCALL_H_

#include <common/types.h>

void sys_exit(i64 exit_code);
void sys_exit_group(i64 exit_code);

i64 sys_getpid(void);
u64 sys_brk(u64 new_brk);

i64 sys_write(i32 fd, u64 user_buf, u64 count);

#endif
