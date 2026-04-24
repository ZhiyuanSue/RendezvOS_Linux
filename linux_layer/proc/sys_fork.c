#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/vspace_copy.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

/*
 * Simplified fork implementation for Linux compatibility.
 *
 * This implements basic fork() semantics:
 * - Creates child process with copied address space
 * - Child returns 0, parent returns child PID
 * - Does NOT implement:
 *   - COW (full page table copy instead)
 *   - File descriptor table sharing
 *   - Signal handler inheritance
 *   - Resource limits
 *
 * Implementation notes:
 * - Uses core's copy_thread() API to create child thread
 * - Child thread resumes from syscall return point with rax=0
 * - Parent's trap_frame is copied to child's kernel stack
 *
 * Limitations:
 * - Only supports single-threaded processes calling fork
 * - No thread-safety guarantees for multi-threaded parents
 * - Copy-on-write not implemented (higher memory usage)
 */

i64 sys_fork()
{
        Tcb_Base *parent = get_cpu_current_task();
        Tcb_Base *child = NULL;
        VS_Common *child_vs = NULL;
        Thread_Base *parent_thread = NULL;
        Thread_Base *child_thread = NULL;
        i64 ret = -LINUX_ENOMEM;
        error_t e;

        if (!parent || !parent->vs) {
                pr_error("[FORK] Invalid parent task\n");
                return -LINUX_ESRCH;
        }

        if (!vs_common_is_table_vspace(parent->vs)) {
                pr_error("[FORK] Parent vspace is not a table vspace\n");
                return -LINUX_EINVAL;
        }

        /* Create child task structure */
        child = new_task_structure(percpu(kallocator), LINUX_PROC_APPEND_BYTES);
        if (!child) {
                pr_error("[FORK] Failed to create child task structure\n");
                ret = -LINUX_ENOMEM;
                goto out;
        }

        /* Copy parent's vspace */
        e = linux_copy_vspace(parent->vs, &child_vs);
        if (e != REND_SUCCESS) {
                pr_error("[FORK] Failed to copy vspace: %d\n", (int)e);
                ret = -LINUX_ENOMEM;
                goto out_free_task;
        }

        child->vs = child_vs;
        child->pid = get_new_id(&pid_manager);

        /* Copy parent's linux_proc_append data */
        linux_proc_append_t *parent_pa = linux_proc_append(parent);
        linux_proc_append_t *child_pa = linux_proc_append(child);
        if (parent_pa && child_pa) {
                memcpy(child_pa, parent_pa, sizeof(*parent_pa));
                /* Child starts with brk at current position */
                child_pa->start_brk = parent_pa->brk;
                /* Set parent PID and initialize exit state */
                child_pa->ppid = parent->pid;
                child_pa->exit_code = 0;
                child_pa->exit_state = 0; /* 0 = running */
                INIT_LIST_HEAD(&child_pa->wait_queue);
        }

        /* Add child to task manager */
        e = add_task_to_manager(percpu(core_tm), child);
        if (e) {
                pr_error("[FORK] Failed to add child to task manager: %d\n", (int)e);
                ret = -LINUX_EAGAIN;
                goto out_free_vspace;
        }

        /*
         * Create child thread using core's copy_thread API.
         * This copies the parent's thread state and prepares
         * the child to resume from the fork syscall with return value 0.
         *
         * We pass LINUX_THREAD_APPEND_BYTES to ensure the child thread
         * has the same layout as the parent (including Linux-specific fields).
         *
         * Note: copy_thread must be called from syscall context on the parent.
         * Core copies the parent's syscall trap frame into the child's save slot
         * inside copy_thread, then run_copied_thread → arch_return_to_user(..., NULL, ret).
         */
        parent_thread = get_cpu_current_thread();
        child_thread = copy_thread(parent_thread, child, 0,
                                   LINUX_THREAD_APPEND_BYTES);
        if (!child_thread) {
                pr_error("[FORK] Failed to create child thread\n");
                ret = -LINUX_ENOMEM;
                goto out_del_from_manager;
        }

        /* Add child thread to task */
        e = add_thread_to_task(child, child_thread);
        if (e) {
                pr_error("[FORK] Failed to add child thread to task: %d\n", (int)e);
                ret = -LINUX_EAGAIN;
                goto out_free_thread;
        }

        /* Add child thread to scheduler */
        e = add_thread_to_manager(percpu(core_tm), child_thread);
        if (e) {
                pr_error("[FORK] Failed to add child thread to scheduler: %d\n", (int)e);
                ret = -LINUX_EAGAIN;
                goto out_del_thread_from_task;
        }

        /* Register child in PID registry */
        e = register_process(child);
        if (e) {
                pr_warn("[FORK] Failed to register child PID: %d\n", (int)e);
                /* Non-fatal: fork still works, just wait/waitpid won't find it */
        }

        pr_info("[FORK] Fork: parent PID=%d, child PID=%d, child tid=%d\n",
                parent->pid, child->pid, child_thread->tid);

        /* Return child PID to parent */
        return (i64)child->pid;

out_del_thread_from_task:
        del_thread_from_task(child_thread);
out_free_thread:
        delete_thread(child_thread);
out_del_from_manager:
        del_task_from_manager(child);
out_free_vspace:
        if (child_vs) {
                child->vs = NULL;
                ref_put(&child_vs->refcount, free_vspace_ref);
        }
out_free_task:
        if (child) {
                delete_task(child);
        }
out:
        return ret;
}
