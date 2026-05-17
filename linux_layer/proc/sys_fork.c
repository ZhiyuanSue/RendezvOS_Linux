#include <common/string.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_deliver.h>
#include <linux_compat/signal/signal_init.h>
#include <rendezvos/trap/trap.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/linux_mm_radix.h>
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
 * - Child thread resumes from the fork syscall with return value 0
 * - Parent's trap_frame is copied to child's kernel stack
 *
 * Limitations:
 * - Only supports single-threaded processes calling fork
 * - No thread-safety guarantees for multi-threaded parents
 * - Copy-on-write not implemented (higher memory usage)
 */

i64 sys_fork(void)
{
        Tcb_Base *parent = get_cpu_current_task();
        Tcb_Base *child = NULL;
        VSpace *child_vs = NULL;
        Thread_Base *parent_thread = NULL;
        Thread_Base *child_thread = NULL;
        i64 ret = -LINUX_ENOMEM;
        error_t e;

        if (!parent || !parent->vs) {
                pr_error("[PROC] fork: Invalid parent task\n");
                return -LINUX_ESRCH;
        }

        if (!linux_vspace_is_user_table(parent->vs)) {
                pr_error("[PROC] fork: Parent has no user vspace (radix/page tables)\n");
                return -LINUX_EINVAL;
        }

        /* Create child task structure */
        child = new_task_structure(percpu(kallocator), LINUX_PROC_APPEND_BYTES);
        if (!child) {
                pr_error("[PROC] fork: Failed to create child task structure\n");
                ret = -LINUX_ENOMEM;
                goto out;
        }

        /* Copy parent's vspace */
        e = linux_copy_vspace(parent->vs, &child_vs);
        if (e != REND_SUCCESS) {
                pr_error("[PROC] fork: Failed to copy vspace: %d\n", (int)e);
                ret = -LINUX_ENOMEM;
                goto out_free_task;
        }

        child->vs = child_vs;
        child->pid = get_new_id(&pid_manager);

        /* Initialize child proc append (do not memcpy wait_queue / exit state). */
        linux_proc_append_t *parent_pa = linux_proc_append(parent);
        linux_proc_append_t *child_pa = linux_proc_append(child);
        if (child_pa) {
                memset(child_pa, 0, sizeof(*child_pa));
                child_pa->ppid = parent->pid;
                child_pa->exit_code = 0;
                child_pa->exit_state = 0;
                INIT_LIST_HEAD(&child_pa->wait_queue);
                if (parent_pa) {
                        child_pa->start_brk = parent_pa->brk;
                        child_pa->brk = parent_pa->brk;
                        child_pa->mmap_hint = parent_pa->mmap_hint;
                        child_pa->pgid = parent_pa->pgid ?
                                                 parent_pa->pgid :
                                                 parent->pid;
                        memcpy(child_pa->signal_dispositions,
                               parent_pa->signal_dispositions,
                               sizeof(child_pa->signal_dispositions));
                }
        }

        /* Add child to task manager */
        e = add_task_to_manager(percpu(core_tm), child);
        if (e != REND_SUCCESS) {
                pr_error(
                        "[PROC] fork: Failed to add child task to task manager: %d\n",
                        (int)e);
                ret = -LINUX_EAGAIN;
                goto out_free_vspace;
        }

        parent_thread = get_cpu_current_thread();

        child_thread =
                copy_thread(parent_thread, child, 0, LINUX_THREAD_APPEND_BYTES);
        if (!child_thread) {
                pr_error("[PROC] fork: Failed to create child thread\n");
                ret = -LINUX_ENOMEM;
                goto out_del_from_manager;
        }

        {
                linux_thread_append_t* parent_ta =
                        linux_thread_append(parent_thread);
                linux_thread_append_t* child_ta =
                        linux_thread_append(child_thread);

                if (child_ta) {
                        linux_signal_init_thread_append(child_ta);
                        if (parent_ta) {
                                child_ta->blocked_signals =
                                        parent_ta->blocked_signals;
                        }
                }
                if (child_pa) {
                        sigemptyset(&child_pa->pending_signals);
                }
        }

        e = add_thread_to_manager(percpu(core_tm), child_thread);
        if (e != REND_SUCCESS) {
                pr_error("[PROC] fork: Failed to add child thread to scheduler: %d\n",
                         (int)e);
                ret = -LINUX_EAGAIN;
                goto out_free_thread;
        }

        e = register_process(child);
        if (e != REND_SUCCESS) {
                pr_warn("[PROC] fork: Failed to register child PID: %d\n", (int)e);
        }

        {
                struct trap_frame* child_tf =
                        (struct trap_frame*)child_thread->kstack_bottom - 1;

                (void)linux_deliver_pending_signals(child_tf);
        }

        pr_info("[PROC] fork: Fork: parent PID=%d, child PID=%d, child tid=%d\n",
                parent->pid,
                child->pid,
                child_thread->tid);

        return (i64)child->pid;

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
