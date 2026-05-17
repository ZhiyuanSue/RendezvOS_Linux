#include <common/string.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/signal/signal_types.h>
#include <linux_compat/signal/signal_queue.h>
#include <modules/log/log.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

/*
 * kill syscall implementation (Phase 2B)
 *
 * Layer A: linux_queue_signal(); layer B: linux_deliver_pending_signals()
 * at syscall/trap return (and child bootstrap trap frame after fork).
 */

i64 sys_kill(i64 pid_i, i64 sig_i)
{
    pid_t pid = (pid_t)pid_i;
    int sig = (int)sig_i;
    Thread_Base *current_thread = get_cpu_current_thread();
    pid_t sender_tid = current_thread ? current_thread->tid : 0;

    pr_info("[KILL] sys_kill called: pid=%d, sig=%d\n", pid, sig);

    /* 基础验证 */
    if (sig < 0 || sig > NSIG) {
        pr_debug("[KILL] Invalid signal number: %d\n", sig);
        return -LINUX_EINVAL;
    }

    /* sig == 0 只做存在性检查 */
    if (sig == 0) {
        Tcb_Base *target = find_task_by_pid(pid);
        return target ? 0 : -LINUX_ESRCH;
    }

    /* 不能发送SIGKILL/SIGSTOP给init */
    if (pid == 1 && (sig == SIGKILL || sig == SIGSTOP)) {
        pr_debug("[KILL] Cannot send SIGKILL/SIGSTOP to init\n");
        return -LINUX_EPERM;
    }

    /* 找到目标进程 */
    Tcb_Base *target = find_task_by_pid(pid);
    if (!target) {
        pr_debug("[KILL] Target process not found: PID=%d\n", pid);
        return -LINUX_ESRCH;
    }

    /* 权限检查（简化版：暂时允许所有信号） */
    /* TODO: 实现正确的权限检查 */

    /* 使用新的信号队列机制 */
    i64 result = linux_queue_signal(target, sig, sender_tid);
    if (result != 0) {
        pr_debug("[KILL] Failed to queue signal: %lld\n", result);
        return result;
    }

    pr_info("[KILL] Successfully queued signal %d for PID=%d\n", sig, pid);
    return 0;
}