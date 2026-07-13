#ifndef _RENDEZVOS_LINUX_SYSCALL_H_
#define _RENDEZVOS_LINUX_SYSCALL_H_

#include <common/types.h>
#include <rendezvos/trap/trap.h>

void sys_exit(i64 exit_code);
void sys_exit_group(i64 exit_code);

i64 sys_getpid(void);
i64 sys_gettid(void);
i64 sys_getppid(void);
i64 sys_getuid(void);
i64 sys_getgid(void);
i64 sys_setuid(u32 uid);
i64 sys_setgid(u32 gid);
u64 sys_brk(u64 new_brk);

i64 sys_write_impl(i32 fd, u64 user_buf, u64 count);
i64 sys_fork();
u64 sys_mmap(u64 addr, u64 length, i64 prot, i64 flags, i64 fd, u64 offset);
i64 sys_munmap(u64 addr, u64 length);
i64 sys_mprotect(u64 addr, u64 length, i64 prot);
i64 sys_mremap(u64 old_address, u64 old_size, u64 new_size, u64 flags,
               u64 new_address);

i64 sys_wait4(i32 pid, u64 user_wstatus, i32 options, u64 user_rusage);
i64 sys_waitid(i32 idtype, u32 id, u64 infop, i32 options);
i64 sys_sched_yield(void);
i64 sys_waitpid(i32 pid, i64* wstatus, i32 options);

i64 sys_clone(u64 flags, u64 stack, u64 parent_tid, u64 child_tid, u64 tls);
i64 sys_set_tid_address(u64 tidptr);
i64 sys_set_robust_list(u64 head_ptr, u64 len);
i64 sys_arch_prctl(i32 code, u64 addr);

i64 sys_kill(i64 pid, i64 sig);
i64 sys_rt_sigaction(i64 signum, u64 act, u64 oldact, u64 sigsetsize);
i64 sys_rt_sigprocmask(i64 how, u64 set, u64 oldset, u64 sigsetsize);
i64 sys_sigaltstack(u64 ss, u64 old_ss);
i64 sys_rt_sigreturn(struct trap_frame* tf);
i64 sys_execve(struct trap_frame* syscall_ctx, u64 user_filename, u64 user_argv,
               u64 user_envp);

/* File system syscalls */
i64 sys_getcwd(u64 user_buf, u64 size);
i64 sys_dup(i32 fd);
i64 sys_dup2(i32 oldfd, i32 newfd);
i64 sys_dup3(i32 oldfd, i32 newfd, i32 flags);
i64 sys_openat(i32 dirfd, u64 user_pathname, i32 flags, u64 mode);
i64 sys_close(i32 fd);
i64 sys_read(i32 fd, u64 user_buf, u64 count);
i64 sys_write(i32 fd, u64 user_buf, u64 count);
i64 sys_ioctl(i32 fd, u32 request, u64 user_arg);
i64 sys_fstat(i32 fd, u64 user_statbuf);
i64 sys_stat(u64 user_pathname, u64 user_statbuf);
i64 sys_newfstatat(i32 dirfd, u64 user_pathname, u64 user_statbuf, i32 flags);
i64 sys_lseek(i32 fd, i64 offset, i32 whence);
i64 sys_chdir(u64 user_pathname);
i64 sys_mkdir(u64 user_pathname, u32 mode);
i64 sys_mkdirat(i32 dirfd, u64 user_pathname, u32 mode);
i64 sys_unlink(u64 user_pathname);
i64 sys_unlinkat(i32 dirfd, u64 user_pathname, i32 flags);
i64 sys_renameat(i32 olddirfd, u64 user_oldpath, i32 newdirfd,
                 u64 user_newpath, u32 flags);
i64 sys_linkat(i32 olddirfd, u64 user_oldpath, i32 newdirfd, u64 user_newpath,
               i32 flags);
i64 sys_getdents64(i32 fd, u64 user_dirp, u64 count);
i64 sys_readlinkat(i32 dirfd, u64 user_pathname, u64 user_buf, u64 bufsiz);
i64 sys_faccessat(i32 dirfd, u64 user_pathname, i32 mode, i32 flags);
i64 sys_pipe(u64 user_pipefd);
i64 sys_pipe2(u64 user_pipefd, i32 flags);
i64 sys_mount(u64 user_source, u64 user_target, u64 user_fstype, u64 flags,
              u64 user_data);
i64 sys_umount2(u64 user_target, i32 flags);

/* Time syscalls */
i64 sys_gettimeofday(u64 user_tv, u64 user_tz);
i64 sys_times(u64 user_buf);
i64 sys_nanosleep(u64 user_req, u64 user_rem);
i64 sys_clock_nanosleep(i32 clockid, i32 flags, u64 user_req, u64 user_rem);
i64 sys_clock_gettime(i32 clockid, u64 user_tp);
i64 sys_time(u64 user_tloc);
i64 sys_uname(u64 user_buf);

i64 sys_getrandom(u64 user_buf, u64 count, u32 flags);
i64 sys_prlimit64(i32 pid, u32 resource, u64 user_new_rlim, u64 user_old_rlim);
i64 sys_rseq(u64 user_rseq, u32 rseq_len, i32 flags, u32 sig);

#endif
