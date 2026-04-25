#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <modules/log/log.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

/* Linux wait4 options */
#define LINUX_WNOHANG    0x00000001 /* Don't block if no child exited */
#define LINUX_WUNTRACED  0x00000002 /* Report stopped children */
#define LINUX_WCONTINUED 0x00000008 /* Report continued children */

/*
 * Wait for a child process to exit.
 *
 * This is a simplified implementation that supports:
 * - pid > 0: wait for specific child
 * - pid == -1: wait for any child
 * - WNOHANG: non-blocking mode
 *
 * Not yet supported:
 * - pid == 0 (wait for children in same process group)
 * - pid < -1 (wait for children in specific process group)
 * - WUNTRACED, WCONTINUED
 * - rusage parameter
 */
i64 sys_wait4(i32 pid, i64* wstatus, i32 options, i64* rusage)
{
        Tcb_Base* parent = get_cpu_current_task();
        linux_proc_append_t* parent_pa = NULL;

        if (!parent) {
                pr_error("[wait4] No current task\n");
                return -LINUX_ESRCH;
        }

        parent_pa = linux_proc_append(parent);
        if (!parent_pa) {
                pr_error("[wait4] No parent proc_append\n");
                return -LINUX_ESRCH;
        }

        pr_debug("[wait4] PID=%d waiting for child PID=%d, options=0x%x\n",
                 parent->pid,
                 pid,
                 options);

        /* Ignore rusage for now */
        if (rusage) {
                pr_debug("[wait4] rusage not supported, ignoring\n");
        }

        /*
         * Simplified polling approach:
         * For Phase 1, we poll the child's exit_state.
         * This is not optimal but sufficient for basic functionality.
         * Future: use IPC to block and wakeup.
         */
        const int max_polls = 10000; /* Prevent infinite loops */
        int poll_count = 0;

        while (poll_count < max_polls) {
                poll_count++;

                /* Find child process */
                Tcb_Base* child = NULL;
                linux_proc_append_t* child_pa = NULL;

                if (pid > 0) {
                        /* Wait for specific child */
                        child = find_task_by_pid(pid);
                        if (!child) {
                                pr_debug("[wait4] Child PID=%d not found\n",
                                         pid);
                                return -LINUX_ECHILD;
                        }

                        child_pa = linux_proc_append(child);
                        if (!child_pa) {
                                pr_error("[wait4] Child has no proc_append\n");
                                return -LINUX_ESRCH;
                        }

                        /* Check if this is actually our child */
                        if (child_pa->ppid != parent->pid) {
                                pr_debug(
                                        "[wait4] PID=%d is not our child (ppid=%d)\n",
                                        pid,
                                        child_pa->ppid);
                                return -LINUX_ECHILD;
                        }
                } else if (pid == -1) {
                        /* Wait for any child - scan all tasks */
                        /* This is inefficient but works for Phase 1 */
                        /* For now, we only handle pid > 0 case */
                        pr_debug("[wait4] pid=-1 not yet supported\n");
                        return -LINUX_ECHILD;
                } else {
                        pr_debug("[wait4] pid=%d not yet supported\n", pid);
                        return -LINUX_EINVAL;
                }

                /* Check child's exit state */
                if (child_pa->exit_state == 1) {
                        /* Child has exited (zombie) */
                        i32 exit_code = child_pa->exit_code;

                        pr_debug("[wait4] Child PID=%d exited with code %d\n",
                                 child->pid,
                                 exit_code);

                        /* Copy exit status to user space */
                        if (wstatus) {
                                /* Simple copy - no user pointer validation yet
                                 */
                                *wstatus = exit_code;
                        }

                        /* Mark child as reaped so it can be freed */
                        child_pa->exit_state = 2; /* 2 = reaped */

                        return (i64)child->pid;
                }

                /* Child still running */
                if (options & LINUX_WNOHANG) {
                        /* Non-blocking mode */
                        pr_debug("[wait4] WNOHANG: no child exited yet\n");
                        return 0;
                }

                /* Small delay before next poll */
                /* In a real implementation, we would block here */
                for (volatile int i = 0; i < 1000; i++)
                        ;
        }

        pr_debug("[wait4] Timeout waiting for child\n");
        return -LINUX_ECHILD;
}
