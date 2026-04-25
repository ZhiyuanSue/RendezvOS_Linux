#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/trap/trap.h>
#include <linux_compat/errno.h>
#include <syscall.h>
#include <syscall_entry.h>

void syscall(struct trap_frame *syscall_ctx)
{
        pr_debug("Syscall id=%d arg1=%llx\n",
                 syscall_ctx->ARCH_SYSCALL_ID,
                 syscall_ctx->ARCH_SYSCALL_ARG_1);
        const u64 syscall_id = (u64)syscall_ctx->ARCH_SYSCALL_ID;
        /* Linux compat: user-visible errors must be Linux errno (negative). */
        i64 ret = -LINUX_ENOSYS;

        switch (syscall_id) {
        case __NR_exit:
                sys_exit(syscall_ctx->ARCH_SYSCALL_ARG_1);
                __builtin_unreachable();
        case __NR_exit_group:
                sys_exit_group((i64)syscall_ctx->ARCH_SYSCALL_ARG_1);
                __builtin_unreachable();
        case __NR_fork:
                ret = sys_fork();
                break;
        case __NR_getpid:
                ret = sys_getpid();
                break;
        case __NR_gettid:
                ret = sys_gettid();
                break;
        case __NR_getppid:
                ret = sys_getppid();
                break;
        case __NR_wait4:
                ret = sys_wait4((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                (i64 *)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                (i32)syscall_ctx->ARCH_SYSCALL_ARG_3,
                                (i64 *)syscall_ctx->ARCH_SYSCALL_ARG_4);
                break;
        case __NR_brk:
                ret = (i64)sys_brk((u64)syscall_ctx->ARCH_SYSCALL_ARG_1);
                break;
        case __NR_write:
                ret = sys_write((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_3);
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
        default:
                pr_debug("[syscall] unimplemented id=%lu\n", (u64)syscall_id);
                break;
        }
        syscall_ctx->ARCH_SYSCALL_RET = (u64)ret;

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
