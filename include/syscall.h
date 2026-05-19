#ifndef _RENDEZVOS_LINUX_SYSCALL_H_
#define _RENDEZVOS_LINUX_SYSCALL_H_

#include <common/types.h>
#include <rendezvos/trap/trap.h>

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
i64 sys_mremap(u64 old_address, u64 old_size, u64 new_size, u64 flags,
               u64 new_address);

i64 sys_wait4(i32 pid, u64 user_wstatus, i32 options, u64 user_rusage);
i64 sys_waitpid(i32 pid, i64* wstatus, i32 options);

i64 sys_clone(u64 flags, u64 stack, u64 parent_tid, u64 child_tid, u64 tls);
i64 sys_set_tid_address(u64 tidptr);
i64 sys_set_robust_list(u64 head_ptr, u64 len);

i64 sys_kill(i64 pid, i64 sig);
i64 sys_rt_sigaction(i64 signum, u64 act, u64 oldact, u64 sigsetsize);
i64 sys_rt_sigprocmask(i64 how, u64 set, u64 oldset, u64 sigsetsize);
i64 sys_sigaltstack(u64 ss, u64 old_ss);
i64 sys_rt_sigreturn(struct trap_frame* tf);
i64 sys_execve(struct trap_frame* syscall_ctx, u64 user_filename, u64 user_argv, u64 user_envp);

#endif
