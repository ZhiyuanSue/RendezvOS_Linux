#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

i64 sys_getpid(void)
{
        Tcb_Base* t = get_cpu_current_task();
        if (!t)
                return -(i64)LINUX_ESRCH;
        return (i64)t->pid;
}

i64 sys_gettid(void)
{
        Thread_Base* t = get_cpu_current_thread();
        if (!t)
                return -(i64)LINUX_ESRCH;

        /*
         * In single-threaded processes, tid == pid.
         * When we implement pthreads, this will return the thread ID.
         */
        return (i64)t->tid;
}

i64 sys_getppid(void)
{
        Tcb_Base* t = get_cpu_current_task();
        linux_proc_append_t* pa = NULL;

        if (!t)
                return -(i64)LINUX_ESRCH;

        pa = linux_proc_append(t);
        if (!pa)
                return -(i64)LINUX_ESRCH;

        return (i64)pa->ppid;
}

i64 sys_waitpid(i32 pid, i64* wstatus, i32 options)
{
        /*
         * waitpid is just a wrapper around wait4 with NULL rusage.
         *
         * According to Linux man page:
         *   pid_t waitpid(pid_t pid, int *wstatus, int options);
         *   pid_t wait4(pid_t pid, int *wstatus, int options, struct rusage *rusage);
         *
         * waitpid(pid, wstatus, options) == wait4(pid, wstatus, options, NULL)
         */
        extern i64 sys_wait4(i32 pid, i64* wstatus, i32 options, i64* rusage);
        return sys_wait4(pid, wstatus, options, NULL);
}
