#include <linux_compat/proc_registry.h>
#include <linux_compat/errno.h>
#include <rendezvos/registry/name_index.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/initcall.h>
#include <common/string.h>
#include <modules/log/log.h>

extern struct Port_Table* global_port_table;

size_t proc_format_pid(char* buf, size_t bufsize, pid_t pid)
{
        u64 v = (u64)pid;
        char rev[32];
        size_t n = 0;

        if (bufsize == 0) {
                return 0;
        }
        if (v == 0) {
                if (bufsize >= 2) {
                        buf[0] = '0';
                        buf[1] = '\0';
                        return 1;
                }
                buf[0] = '\0';
                return 0;
        }
        while (v > 0 && n < sizeof(rev)) {
                rev[n++] = (char)('0' + (v % 10U));
                v /= 10U;
        }
        if (n + 1 > bufsize) {
                buf[0] = '\0';
                return 0;
        }
        for (size_t i = 0; i < n; i++) {
                buf[i] = rev[n - 1 - i];
        }
        buf[n] = '\0';
        return n;
}

size_t proc_format_wait_port_name(char* buf, size_t bufsize, pid_t pid)
{
        static const char prefix[] = "wait_port_";
        size_t i;

        if (bufsize < sizeof(prefix)) {
                if (bufsize > 0) {
                        buf[0] = '\0';
                }
                return 0;
        }
        for (i = 0; i < sizeof(prefix) - 1; i++) {
                buf[i] = prefix[i];
        }
        {
                size_t pid_len = proc_format_pid(buf + i, bufsize - i, pid);

                if (pid_len == 0) {
                        buf[0] = '\0';
                        return 0;
                }
                return i + pid_len;
        }
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

        proc_format_pid(name_buf, sizeof(name_buf), task->pid);
        return name_buf;
}

/* name_index hold: core has no Tcb refcount; pin is registry lifetime only. */
static bool proc_task_hold_helper(void* value)
{
        Tcb_Base* task = (Tcb_Base*)value;

        return task != NULL && task->pid > 0;
}

static void proc_task_drop_helper(void* value)
{
        (void)value;
}

Message_Port_t* proc_get_or_create_wait_port(pid_t pid)
{
        char port_name[PROC_WAIT_PORT_NAME_MAX];
        Message_Port_t* port;

        if (pid <= 0) {
                return NULL;
        }
        if (proc_format_wait_port_name(port_name, sizeof(port_name), pid)
            == 0) {
                return NULL;
        }

        port = thread_lookup_port(port_name);
        if (port) {
                return port;
        }

        port = create_message_port(port_name);
        if (!port) {
                return NULL;
        }
        if (register_port(global_port_table, port) != REND_SUCCESS) {
                delete_message_port_structure(port);
                return NULL;
        }
        return port;
}

void proc_registry_init(void)
{
        name_index_init(&pid_index,
                        NULL,
                        16,
                        NULL,
                        task_get_name,
                        proc_task_hold_helper,
                        proc_task_drop_helper,
                        NULL,
                        NULL);
        pr_info("[PROC] PID registry initialized\n");
}

/*
 * Remove every registry row for this decimal PID name.
 * name_index_register does not replace an existing name; stale rows must be
 * cleared explicitly (unregister_process used to pass row_idx=0 and no-op).
 */
static void proc_registry_evict_pid(pid_t pid, Tcb_Base* only_task)
{
        char name_buf[16];

        if (pid <= 0) {
                return;
        }

        proc_format_pid(name_buf, sizeof(name_buf), pid);

        for (;;) {
                name_index_token_t tok;
                Tcb_Base* existing = (Tcb_Base*)name_index_lookup(
                        &pid_index, name_buf, &tok);
                if (!existing
                    || tok.row_index == NAME_INDEX_ROW_INDEX_INVALID) {
                        return;
                }
                if (only_task && existing != only_task) {
                        /*
                         * Stale row left by the old row_idx=0 unregister bug;
                         * drop it and keep scanning for only_task.
                         */
                        name_index_unregister(&pid_index,
                                              existing,
                                              (u64)tok.row_index,
                                              name_buf);
                        continue;
                }
                name_index_unregister(
                        &pid_index, existing, (u64)tok.row_index, name_buf);
        }
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

        proc_registry_evict_pid(task->pid, NULL);

        u64 row_idx;
        error_t e = name_index_register(&pid_index, task, &row_idx);
        if (e != REND_SUCCESS) {
                pr_error("[proc] Failed to register PID %d: %d\n",
                         task->pid,
                         (int)e);
                return -LINUX_EAGAIN;
        }

        return REND_SUCCESS;
}

Tcb_Base* find_task_by_pid(pid_t pid)
{
        if (pid <= 0) {
                return NULL;
        }

        char name_buf[16];
        proc_format_pid(name_buf, sizeof(name_buf), pid);

        return (Tcb_Base*)name_index_lookup(&pid_index, name_buf, NULL);
}

void unregister_process(Tcb_Base* task)
{
        if (!task || task->pid == INVALID_ID) {
                return;
        }

        proc_registry_evict_pid(task->pid, task);
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

        /* Try a reasonable range of PIDs (assuming PIDs are allocated
         * sequentially) */
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

                /* Check if this is our child, in zombie state, and matches pgid
                 */
                if (pa->ppid == ppid && pa->exit_state == 1
                    && pa->pgid == pgid) {
                        return child;
                }
        }

        return NULL;
}

bool proc_parent_has_unreaped_child(pid_t ppid, pid_t pgid, bool filter_by_pgid)
{
        if (ppid <= 0) {
                return false;
        }
        if (filter_by_pgid && pgid <= 0) {
                return false;
        }

        for (pid_t candidate = ppid + 1; candidate < ppid + 1000; candidate++) {
                Tcb_Base* child = find_task_by_pid(candidate);
                linux_proc_append_t* pa;

                if (!child) {
                        continue;
                }

                pa = linux_proc_append(child);
                if (!pa || pa->ppid != ppid || pa->exit_state == 2) {
                        continue;
                }
                if (filter_by_pgid && pa->pgid != pgid) {
                        continue;
                }
                return true;
        }

        return false;
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
