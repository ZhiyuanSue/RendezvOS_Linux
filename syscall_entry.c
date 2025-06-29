#include <modules/RendezvOS_Linux/syscall_entry.h>
#include <modules/log/log.h>

void syscall(struct trap_frame* syscall_ctx)
{
        pr_info("Syscall with id %d arg1: %d\n",
                syscall_ctx->ARCH_SYSCALL_ID,
                syscall_ctx->ARCH_SYSCALL_ARG_1);
        switch (syscall_ctx->ARCH_SYSCALL_ID) {
        case 60:
                break;
        default:
                break;
        }
        schedule(percpu(core_tm));
        return;
}
static inline void
set_syscall_entry(void (*syscall_entry)(struct trap_frame* syscall_ctx))
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