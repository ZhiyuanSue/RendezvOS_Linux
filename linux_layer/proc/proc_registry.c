#include <linux_compat/proc_registry.h>
#include <linux_compat/errno.h>
#include <rendezvos/registry/name_index.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/initcall.h>
#include <common/string.h>
#include <modules/log/log.h>

/*
 * Decimal string for PID (no libc snprintf).
 * buf must hold at least 2 bytes for pid == 0; otherwise NUL-terminated.
 */
static void format_pid_decimal(char* buf, size_t bufsize, pid_t pid)
{
        u64 v = (u64)pid;
        char rev[32];
        size_t n = 0;

        if (bufsize == 0) {
                return;
        }
        if (v == 0) {
                if (bufsize >= 2) {
                        buf[0] = '0';
                        buf[1] = '\0';
                } else {
                        buf[0] = '\0';
                }
                return;
        }
        while (v > 0 && n < sizeof(rev)) {
                rev[n++] = (char)('0' + (v % 10U));
                v /= 10U;
        }
        if (n + 1 > bufsize) {
                buf[0] = '\0';
                return;
        }
        for (size_t i = 0; i < n; i++) {
                buf[i] = rev[n - 1 - i];
        }
        buf[n] = '\0';
}

/*
 * Process registry using core's name_index mechanism.
 *
 * This provides O(1) PID lookup for wait/waitpid syscalls.
 * The PID is converted to a string name for the name_index.
 */

static name_index_t pid_index;

/*
 * Get task name as PID string for name_index.
 *
 * Note: This returns a pointer to a static buffer, which is safe
 * because name_index_get_name is called under lock and the result
 * is used immediately.
 */
static const char* task_get_name(void* value)
{
        static char name_buf[16];
        Tcb_Base* task = (Tcb_Base*)value;

        if (task->pid == INVALID_ID) {
                return NULL;
        }

        format_pid_decimal(name_buf, sizeof(name_buf), task->pid);
        return name_buf;
}

/*
 * Hold callback - increment task refcount.
 */
static bool task_hold(void* value)
{
        Tcb_Base* task = (Tcb_Base*)value;
        /* TODO: Implement task refcount if needed */
        (void)task;
        return true;
}

/*
 * Drop callback - decrement task refcount.
 */
static void task_drop(void* value)
{
        Tcb_Base* task = (Tcb_Base*)value;
        /* TODO: Implement task refcount if needed */
        (void)task;
}

void proc_registry_init(void)
{
        name_index_init(&pid_index,
                        NULL,
                        16,
                        NULL,
                        task_get_name,
                        task_hold,
                        task_drop,
                        NULL,
                        NULL);
        pr_info("[proc] PID registry initialized\n");
}

error_t register_process(Tcb_Base* task)
{
        if (!task) {
                return -LINUX_EINVAL;
        }

        if (task->pid == INVALID_ID) {
                pr_error("[proc] Cannot register task with invalid PID\n");
                return -LINUX_EINVAL;
        }

        u64 row_idx;
        error_t e = name_index_register(&pid_index, task, &row_idx);
        if (e) {
                pr_error("[proc] Failed to register PID %d: %d\n",
                         task->pid,
                         (int)e);
                return -LINUX_EAGAIN;
        }

        pr_debug("[proc] Registered PID %d\n", task->pid);
        return REND_SUCCESS;
}

Tcb_Base* find_task_by_pid(pid_t pid)
{
        if (pid <= 0) {
                return NULL;
        }

        char name_buf[16];
        format_pid_decimal(name_buf, sizeof(name_buf), pid);

        Tcb_Base* task =
                (Tcb_Base*)name_index_lookup(&pid_index, name_buf, NULL);
        if (task) {
                pr_debug("[proc] Found PID %d\n", pid);
        } else {
                pr_debug("[proc] PID %d not found\n", pid);
        }

        return task;
}

void unregister_process(Tcb_Base* task)
{
        if (!task || task->pid == INVALID_ID) {
                return;
        }

        char name_buf[16];
        format_pid_decimal(name_buf, sizeof(name_buf), task->pid);

        /*
         * TODO: We need to store the row_idx when registering to
         * properly unregister. For now, this is a simplified version.
         */
        name_index_unregister(&pid_index, task, 0, name_buf);
        pr_debug("[proc] Unregistered PID %d\n", task->pid);
}

/*
 * Find a zombie child by parent PID.
 * This implements the lookup needed for wait4(pid == -1).
 *
 * Strategy: Try PID ranges sequentially until we find a zombie child.
 * This is O(N) in the number of PIDs, but N is typically small.
 * TODO: Optimize with reverse index if needed.
 */
Tcb_Base* find_zombie_child(pid_t ppid)
{
        if (ppid <= 0) {
                return NULL;
        }

        /* Try a reasonable range of PIDs (assuming PIDs are allocated sequentially) */
        for (pid_t candidate = ppid + 1; candidate < ppid + 1000; candidate++) {
                Tcb_Base* child = find_task_by_pid(candidate);
                if (!child) {
                        continue;
                }

                linux_proc_append_t* pa = linux_proc_append(child);
                if (!pa) {
                        continue;
                }

                /* Check if this is our child and in zombie state */
                if (pa->ppid == ppid && pa->exit_state == 1) {
                        pr_debug("[proc] Found zombie child PID=%d of parent PID=%d\n",
                                 candidate, ppid);
                        return child;
                }
        }

        return NULL;
}

/*
 * Find a zombie child in the same process group.
 * This implements the lookup needed for wait4(pid == 0) and wait4(pid < -1).
 */
Tcb_Base* find_zombie_child_in_pgid(pid_t ppid, pid_t pgid)
{
        if (ppid <= 0 || pgid <= 0) {
                return NULL;
        }

        /* Try a reasonable range of PIDs */
        for (pid_t candidate = ppid + 1; candidate < ppid + 1000; candidate++) {
                Tcb_Base* child = find_task_by_pid(candidate);
                if (!child) {
                        continue;
                }

                linux_proc_append_t* pa = linux_proc_append(child);
                if (!pa) {
                        continue;
                }

                /* Check if this is our child, in zombie state, and matches pgid */
                if (pa->ppid == ppid && pa->exit_state == 1 && pa->pgid == pgid) {
                        pr_debug("[proc] Found zombie child PID=%d of parent PID=%d in pgid %d\n",
                                 candidate, ppid, pgid);
                        return child;
                }
        }

        return NULL;
}

/*
 * Initcall to initialize the process registry.
 * Runs at init level 2 (early init, before user programs).
 */
static void proc_registry_initcall(void)
{
        proc_registry_init();
}
DEFINE_INIT(proc_registry_initcall);
