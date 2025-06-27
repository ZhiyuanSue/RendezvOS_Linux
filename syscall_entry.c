#include <modules/RendezvOS_Linux/syscall_entry.h>
#include <modules/log/log.h>

void syscall(Arch_Syscall_Context* syscall_ctx)
{
        pr_info("Syscall with id %d arg1: %d\n",
                syscall_ctx->syscall_id,
                syscall_ctx->arg1);
        switch (syscall_ctx->syscall_id) {
        case 60:
                schedule(percpu(core_tm));
                break;

        default:
                break;
        }
        return;
}
static inline void
set_syscall_entry(void (*syscall_entry)(Arch_Syscall_Context* syscall_ctx))
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