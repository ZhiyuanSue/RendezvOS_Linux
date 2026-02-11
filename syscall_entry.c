#include <modules/RendezvOS_Linux/syscall_entry.h>
#include <modules/log/log.h>
#include <rendezvos/task/tcb.h>

void syscall(struct trap_frame* syscall_ctx)
{
        pr_info("Syscall with id %d arg1: %d\n",
                syscall_ctx->ARCH_SYSCALL_ID,
                syscall_ctx->ARCH_SYSCALL_ARG_1);
        Thread_Base* curr = get_cpu_current_thread();
        switch (syscall_ctx->ARCH_SYSCALL_ID) {
        case 60:
                // Thread_Base* curr_thread = percpu(core_tm)->current_thread;
                // delete_thread(curr_thread);
                // Tcb_Base* curr_tcb = percpu(core_tm)->current_task;
                // if(!(curr_tcb->thread_number))
                //         delete_task(curr_tcb);
                thread_set_status(curr, thread_status_zombie);
                break;
        default:
                pr_error("[ Syscall ] get a undefined syscall id %d\n",syscall_ctx->ARCH_SYSCALL_ID);
                thread_set_status(curr,thread_status_zombie);
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