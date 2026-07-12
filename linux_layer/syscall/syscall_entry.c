#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/trap/trap.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/signal/signal_deliver.h>
#include <syscall.h>
#include <syscall_entry.h>

void syscall(struct trap_frame *syscall_ctx)
{
        const u64 syscall_id = (u64)syscall_ctx->ARCH_SYSCALL_ID;
        /* Linux compat: user-visible errors must be Linux errno (negative). */
        i64 ret = -LINUX_ENOSYS;
        bool skip_syscall_ret_assign = false;
        bool skip_signal_deliver = false;

        switch (syscall_id) {
        case __NR_exit:
                sys_exit(syscall_ctx->ARCH_SYSCALL_ARG_1);
                __builtin_unreachable();
        case __NR_exit_group:
                sys_exit_group((i64)syscall_ctx->ARCH_SYSCALL_ARG_1);
                __builtin_unreachable();
        case __NR_clone:
#if defined(_X86_64_)
        case __NR_fork:
#endif
        {
                /* Handle clone/fork - on aarch64 fork is clone(0, SIGCHLD) */
                u64 clone_flags = (u64)syscall_ctx->ARCH_SYSCALL_ARG_1;
#if defined(_X86_64_)
                /* x86_64: separate syscalls */
                if (syscall_id == __NR_fork) {
                        ret = sys_fork();
                } else {
                        ret = sys_clone(clone_flags,
                                        (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                        (u64)syscall_ctx->ARCH_SYSCALL_ARG_3,
                                        (u64)syscall_ctx->ARCH_SYSCALL_ARG_4,
                                        (u64)syscall_ctx->ARCH_SYSCALL_ARG_5);
                }
#elif defined(_AARCH64_)
                /* aarch64: fork is clone with flags=0 */
                ret = sys_clone(clone_flags,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_3,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_4,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_5);
#else
#error "Unsupported architecture"
#endif
                break;
        }
        case __NR_getpid:
                ret = sys_getpid();
                break;
        case __NR_gettid:
                ret = sys_gettid();
                break;
        case __NR_set_tid_address:
                ret = sys_set_tid_address(syscall_ctx->ARCH_SYSCALL_ARG_1);
                break;
        case __NR_set_robust_list:
                ret = sys_set_robust_list(syscall_ctx->ARCH_SYSCALL_ARG_1,
                                          syscall_ctx->ARCH_SYSCALL_ARG_2);
                break;
        case __NR_getppid:
                ret = sys_getppid();
                break;
        case __NR_kill:
                ret = sys_kill((i64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                               (i64)syscall_ctx->ARCH_SYSCALL_ARG_2);
                break;
        case __NR_rt_sigaction:
                ret = sys_rt_sigaction((i64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                       syscall_ctx->ARCH_SYSCALL_ARG_2,
                                       syscall_ctx->ARCH_SYSCALL_ARG_3,
                                       syscall_ctx->ARCH_SYSCALL_ARG_4);
                break;
        case __NR_rt_sigprocmask:
                ret = sys_rt_sigprocmask((i64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                         syscall_ctx->ARCH_SYSCALL_ARG_2,
                                         syscall_ctx->ARCH_SYSCALL_ARG_3,
                                         syscall_ctx->ARCH_SYSCALL_ARG_4);
                break;
        case __NR_sigaltstack:
                ret = sys_sigaltstack(syscall_ctx->ARCH_SYSCALL_ARG_1,
                                      syscall_ctx->ARCH_SYSCALL_ARG_2);
                break;
        case __NR_rt_sigreturn:
                ret = sys_rt_sigreturn(syscall_ctx);
                if (ret == 0) {
                        skip_syscall_ret_assign = true;
                        skip_signal_deliver = true;
                }
                break;
        case __NR_wait4:
                ret = sys_wait4((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                syscall_ctx->ARCH_SYSCALL_ARG_2,
                                (i32)syscall_ctx->ARCH_SYSCALL_ARG_3,
                                syscall_ctx->ARCH_SYSCALL_ARG_4);
                break;
        case __NR_waitid:
                ret = sys_waitid((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                 (u32)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                 syscall_ctx->ARCH_SYSCALL_ARG_3,
                                 (i32)syscall_ctx->ARCH_SYSCALL_ARG_4);
                break;
        case __NR_sched_yield:
                ret = sys_sched_yield();
                break;
#if defined(_X86_64_)
        case __NR_nanosleep:
                ret = sys_nanosleep((u64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                    (u64)syscall_ctx->ARCH_SYSCALL_ARG_2);
                break;
        case __NR_clock_nanosleep:
                ret = sys_clock_nanosleep((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                          (i32)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                          (u64)syscall_ctx->ARCH_SYSCALL_ARG_3,
                                          (u64)syscall_ctx->ARCH_SYSCALL_ARG_4);
                break;
#elif defined(_AARCH64_)
        case __NR_nanosleep:
                ret = sys_nanosleep((u64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                    (u64)syscall_ctx->ARCH_SYSCALL_ARG_2);
                break;
        case __NR_clock_nanosleep:
                ret = sys_clock_nanosleep((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                          (i32)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                          (u64)syscall_ctx->ARCH_SYSCALL_ARG_3,
                                          (u64)syscall_ctx->ARCH_SYSCALL_ARG_4);
                break;
#endif
        case __NR_gettimeofday:
                ret = sys_gettimeofday((u64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                       (u64)syscall_ctx->ARCH_SYSCALL_ARG_2);
                break;
        case __NR_times:
                ret = sys_times((u64)syscall_ctx->ARCH_SYSCALL_ARG_1);
                break;
        case __NR_uname:
                ret = sys_uname((u64)syscall_ctx->ARCH_SYSCALL_ARG_1);
                break;
        case __NR_clock_gettime:
                ret = sys_clock_gettime((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                        (u64)syscall_ctx->ARCH_SYSCALL_ARG_2);
                break;
        case __NR_brk:
                ret = (i64)sys_brk((u64)syscall_ctx->ARCH_SYSCALL_ARG_1);
                break;
        case __NR_write:
                ret = sys_write((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_3);
                break;
        case __NR_read:
                ret = sys_read((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                               (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                               (u64)syscall_ctx->ARCH_SYSCALL_ARG_3);
                break;
        case __NR_close:
                ret = sys_close((i32)syscall_ctx->ARCH_SYSCALL_ARG_1);
                break;
        case __NR_dup:
                ret = sys_dup((i32)syscall_ctx->ARCH_SYSCALL_ARG_1);
                break;
#if defined(_X86_64_)
        case __NR_dup2:
                ret = sys_dup2((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                               (i32)syscall_ctx->ARCH_SYSCALL_ARG_2);
                break;
#endif
        case __NR_dup3:
                ret = sys_dup3((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                               (i32)syscall_ctx->ARCH_SYSCALL_ARG_2,
                               (i32)syscall_ctx->ARCH_SYSCALL_ARG_3);
                break;
        case __NR_fstat:
                ret = sys_fstat((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_2);
                break;
#if defined(_X86_64_)
        case __NR_stat:
                ret = sys_stat((u64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                               (u64)syscall_ctx->ARCH_SYSCALL_ARG_2);
                break;
#endif
        case __NR_newfstatat:
                ret = sys_newfstatat((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                     (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                     (u64)syscall_ctx->ARCH_SYSCALL_ARG_3,
                                     (i32)syscall_ctx->ARCH_SYSCALL_ARG_4);
                break;
        case __NR_readlinkat:
                ret = sys_readlinkat((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                     (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                     (u64)syscall_ctx->ARCH_SYSCALL_ARG_3,
                                     (u64)syscall_ctx->ARCH_SYSCALL_ARG_4);
                break;
        case __NR_faccessat:
                ret = sys_faccessat((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                    (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                    (i32)syscall_ctx->ARCH_SYSCALL_ARG_3,
                                    (i32)syscall_ctx->ARCH_SYSCALL_ARG_4);
                break;
        case __NR_lseek:
                ret = sys_lseek((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                (i64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                (i32)syscall_ctx->ARCH_SYSCALL_ARG_3);
                break;
        case __NR_getcwd:
                ret = sys_getcwd((u64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                 (u64)syscall_ctx->ARCH_SYSCALL_ARG_2);
                break;
        case __NR_chdir:
                ret = sys_chdir((u64)syscall_ctx->ARCH_SYSCALL_ARG_1);
                break;
#if defined(_X86_64_)
        case __NR_mkdir:
                ret = sys_mkdir((u64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                (u32)syscall_ctx->ARCH_SYSCALL_ARG_2);
                break;
#endif
        case __NR_mkdirat:
                ret = sys_mkdirat((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                  (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                  (u32)syscall_ctx->ARCH_SYSCALL_ARG_3);
                break;
#if defined(_X86_64_)
        case __NR_unlink:
                ret = sys_unlink((u64)syscall_ctx->ARCH_SYSCALL_ARG_1);
                break;
#endif
        case __NR_unlinkat:
                ret = sys_unlinkat((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                   (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                   (i32)syscall_ctx->ARCH_SYSCALL_ARG_3);
                break;
        case __NR_renameat:
                ret = sys_renameat((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                   (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                   (i32)syscall_ctx->ARCH_SYSCALL_ARG_3,
                                   (u64)syscall_ctx->ARCH_SYSCALL_ARG_4,
                                   0u);
                break;
        case __NR_renameat2:
                ret = sys_renameat((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                   (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                   (i32)syscall_ctx->ARCH_SYSCALL_ARG_3,
                                   (u64)syscall_ctx->ARCH_SYSCALL_ARG_4,
                                   (u32)syscall_ctx->ARCH_SYSCALL_ARG_5);
                break;
        case __NR_linkat:
                ret = sys_linkat((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                 (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                 (i32)syscall_ctx->ARCH_SYSCALL_ARG_3,
                                 (u64)syscall_ctx->ARCH_SYSCALL_ARG_4,
                                 (i32)syscall_ctx->ARCH_SYSCALL_ARG_5);
                break;
        case __NR_getdents64:
                ret = sys_getdents64((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                     (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                     (u64)syscall_ctx->ARCH_SYSCALL_ARG_3);
                break;
#if defined(_X86_64_)
        case __NR_pipe:
                ret = sys_pipe((u64)syscall_ctx->ARCH_SYSCALL_ARG_1);
                break;
#endif
        case __NR_pipe2:
                ret = sys_pipe2((u64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                (i32)syscall_ctx->ARCH_SYSCALL_ARG_2);
                break;
        case __NR_mmap:
                ret = (i64)sys_mmap((u64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                    (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                    (i64)syscall_ctx->ARCH_SYSCALL_ARG_3,
                                    (i64)syscall_ctx->ARCH_SYSCALL_ARG_4,
                                    (i64)syscall_ctx->ARCH_SYSCALL_ARG_5,
                                    (u64)syscall_ctx->ARCH_SYSCALL_ARG_6);
                break;
        case __NR_munmap:
                ret = sys_munmap((u64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                 (u64)syscall_ctx->ARCH_SYSCALL_ARG_2);
                break;
        case __NR_mprotect:
                ret = sys_mprotect((u64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                   (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                   (i64)syscall_ctx->ARCH_SYSCALL_ARG_3);
                break;
        case __NR_mremap:
                ret = sys_mremap((u64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                 (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                 (u64)syscall_ctx->ARCH_SYSCALL_ARG_3,
                                 (u64)syscall_ctx->ARCH_SYSCALL_ARG_4,
                                 (u64)syscall_ctx->ARCH_SYSCALL_ARG_5);
                break;
        case __NR_execve:
                ret = sys_execve(syscall_ctx,
                                 (u64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                 (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                 (u64)syscall_ctx->ARCH_SYSCALL_ARG_3);
                if (ret == 0) {
                        skip_syscall_ret_assign = true;
                        skip_signal_deliver = true;
                }
                break;
        case __NR_openat:
                ret = sys_openat((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                 (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                 (i32)syscall_ctx->ARCH_SYSCALL_ARG_3,
                                 (u64)syscall_ctx->ARCH_SYSCALL_ARG_4);
                break;
#if defined(_X86_64_)
        case __NR_open:
                ret = sys_openat(LINUX_AT_FDCWD,
                                 (u64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                 (i32)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                 (u64)syscall_ctx->ARCH_SYSCALL_ARG_3);
                break;
#endif
#if defined(_X86_64_)
        case __NR_mount:
                ret = sys_mount((u64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_3,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_4,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_5);
                break;
        case __NR_umount2:
                ret = sys_umount2((u64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                  (i32)syscall_ctx->ARCH_SYSCALL_ARG_2);
                break;
#elif defined(_AARCH64_)
        case __NR_mount:
                ret = sys_mount((u64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_3,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_4,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_5);
                break;
        case __NR_umount2:
                ret = sys_umount2((u64)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                  (i32)syscall_ctx->ARCH_SYSCALL_ARG_2);
                break;
#endif
#if defined(_X86_64_)
        case __NR_arch_prctl:
                ret = sys_arch_prctl((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                     (u64)syscall_ctx->ARCH_SYSCALL_ARG_2);
                break;
#endif
        default:
                pr_debug("[SYSCALL] unimplemented id=%lu\n", (u64)syscall_id);
                break;
        }

        if (!skip_syscall_ret_assign) {
                syscall_ctx->ARCH_SYSCALL_RET = (u64)ret;
        }

        /* Deliver after syscall return value is set (handler uses rdi/x0, not
         * ret reg). */
        if (!skip_signal_deliver) {
                (void)linux_deliver_pending_signals(syscall_ctx);
        }

        return;
}
static inline void
set_syscall_entry(void (*syscall_entry)(struct trap_frame *syscall_ctx))
{
        if (!syscall_entry) {
                pr_error("[Error] no syscall entry is defined\n");
                return;
        }
        return;
}
void init_syscall_entry()
{
        set_syscall_entry(syscall);
}
