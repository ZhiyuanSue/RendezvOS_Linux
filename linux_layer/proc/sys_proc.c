#include <common/types.h>
#include <linux_compat/errno.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

i64 sys_getpid(void)
{
        Tcb_Base* t = get_cpu_current_task();
        if (!t)
                return -(i64)LINUX_ESRCH;
        return (i64)t->pid;
}
