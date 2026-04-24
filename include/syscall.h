#ifndef _RENDEZVOS_LINUX_SYSCALL_H_
#define _RENDEZVOS_LINUX_SYSCALL_H_

#include <common/types.h>

void sys_exit(i64 exit_code);
void sys_exit_group(i64 exit_code);

i64 sys_getpid(void);
i64 sys_gettid(void);
i64 sys_getppid(void);
u64 sys_brk(u64 new_brk);

i64 sys_write(i32 fd, u64 user_buf, u64 count);
i64 sys_fork();
u64 sys_mmap(u64 addr, u64 length, i64 prot, i64 flags, i64 fd, u64 offset);
i64 sys_munmap(u64 addr, u64 length);
i64 sys_mprotect(u64 addr, u64 length, i64 prot);
i64 sys_mremap(u64 old_address,
              u64 old_size,
              u64 new_size,
              u64 flags,
              u64 new_address);

i64 sys_wait4(i32 pid, i64* wstatus, i32 options, i64* rusage);
i64 sys_waitpid(i32 pid, i64* wstatus, i32 options);

#endif
