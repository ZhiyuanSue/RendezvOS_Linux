#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>
#include <linux_compat/errno.h>
#include <syscall.h>
#include <syscall_entry.h>

void syscall(struct trap_frame *syscall_ctx)
{
        pr_info("Syscall with id %d arg1: %llx\n",
                syscall_ctx->ARCH_SYSCALL_ID,
                syscall_ctx->ARCH_SYSCALL_ARG_1);
        const u64 syscall_id = (u64)syscall_ctx->ARCH_SYSCALL_ID;
        Thread_Base *curr = get_cpu_current_thread();
        /* Linux compat: user-visible errors must be Linux errno (negative). */
        i64 ret = -LINUX_ENOSYS;

        switch (syscall_id) {
        case __NR_exit:
                sys_exit(syscall_ctx->ARCH_SYSCALL_ARG_1);
                break;
        case __NR_exit_group:
                sys_exit_group((i64)syscall_ctx->ARCH_SYSCALL_ARG_1);
                break;
        case __NR_getpid:
                ret = sys_getpid();
                break;
        case __NR_brk:
                ret = (i64)sys_brk((u64)syscall_ctx->ARCH_SYSCALL_ARG_1);
                break;
        case __NR_write:
                ret = sys_write((i32)syscall_ctx->ARCH_SYSCALL_ARG_1,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_2,
                                (u64)syscall_ctx->ARCH_SYSCALL_ARG_3);
                break;
        default:
                /* Default policy: kill the calling thread to force visibility.
                 */
                if (curr) {
                        pr_error("[syscall] unimplemented id=%lu arg1=%lu\n",
                                 (u64)syscall_id,
                                 (u64)syscall_ctx->ARCH_SYSCALL_ARG_1);
                        sys_exit(-1);
                }
                break;
        }
        syscall_ctx->ARCH_SYSCALL_RET = (u64)ret;
        schedule(percpu(core_tm));
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
