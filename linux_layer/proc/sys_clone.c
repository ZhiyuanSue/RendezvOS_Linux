#include <common/string.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/append_hooks.h>
#include <linux_compat/clone_flags.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/vspace_copy.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/vmm.h>
#include <common/refcount.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

/*
 * Clone syscall implementation for Linux compatibility.
 *
 * This implements clone() for thread creation with the following flags:
 * - CLONE_VM: Share address space (required for threads)
 * - CLONE_FS: Share filesystem information
 * - CLONE_FILES: Share file descriptor table
 * - CLONE_SIGHAND: Share signal handlers
 * - CLONE_THREAD: Same thread group
 * - CLONE_SETTLS: Set thread-local storage
 * - CLONE_PARENT_SETTID: Store child TID in parent memory
 * - CLONE_CHILD_SETTID: Store child TID in child memory
 * - CLONE_CHILD_CLEARTID: Clear TID on exit (via set_tid_address)
 *
 * Raw syscall signature (x86_64):
 * long clone(unsigned long flags, void *stack,
 *            int *parent_tid, int *child_tid,
 *            unsigned long tls);
 *
 * Implementation notes:
 * - Append: memset + proc static fields, then linux_task_append_clone (see APPEND_HOOKS.md)
 * - Reuses copy_thread() from core (thread.append_hooks.copy)
 * - With CLONE_VM: shares parent's VSpace (no refcount increment needed)
 * - With CLONE_THREAD: sets same thread group (stored in proc_append)
 * - TLS setup via CLONE_SETTLS (architecture-specific)
 * - Returns child TID to parent, 0 to child
 *
 * Limitations:
 * - CLONE_FS/CLONE_FILES/CLONE_SIGHAND are placeholders for Phase 2B/2C
 * - TLS implementation is architecture-specific (x86_64 only for now)
 * - No namespace support (CLONE_NEW*)
 */

/*
 * Validate clone flag combinations.
 * Returns 0 if valid, -LINUX_EINVAL if invalid.
 */
static i64 validate_clone_flags(u64 flags)
{
        /* CLONE_SIGHAND requires CLONE_VM */
        if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM)) {
                return -LINUX_EINVAL;
        }

        /* CLONE_THREAD requires CLONE_SIGHAND (which requires CLONE_VM) */
        if ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND)) {
                return -LINUX_EINVAL;
        }

        /* Thread creation requires CLONE_VM */
        if ((flags & CLONE_THREAD) && !(flags & CLONE_VM)) {
                return -LINUX_EINVAL;
        }

        return 0;
}

i64 sys_clone(u64 flags, u64 stack, u64 parent_tid, u64 child_tid, u64 tls)
{
        Tcb_Base *parent = get_cpu_current_task();
        Tcb_Base *child = NULL;
        Thread_Base *parent_thread = NULL;
        Thread_Base *child_thread = NULL;
        VSpace *child_vs = NULL;
        i64 ret;
        error_t e;

        if (!parent || !parent->vs) {
                pr_error("[PROC] clone: Invalid parent task\n");
                return -LINUX_ESRCH;
        }

        /* Validate flag combinations */
        ret = validate_clone_flags(flags);
        if (ret != 0) {
                return ret;
        }

        /* Validate stack pointer */
        if ((flags & CLONE_VM) && stack == 0) {
                return -LINUX_EINVAL;
        }

        parent_thread = get_cpu_current_thread();
        if (!parent_thread) {
                pr_error("[PROC] clone: Invalid parent thread\n");
                return -LINUX_ESRCH;
        }

        /*
         * For thread creation (CLONE_VM), we share the address space.
         * For process creation (no CLONE_VM), we copy the address space (like
         * fork).
         */
        if (flags & CLONE_VM) {
                if (!ref_get_not_zero(&parent->vs->refcount)) {
                        pr_error(
                                "[PROC] clone: parent vspace refcount invalid\n");
                        return -LINUX_ESRCH;
                }
                child_vs = parent->vs;
        } else {
                /* Copy parent's VSpace */
                e = linux_copy_vspace(parent->vs, &child_vs);
                if (e != REND_SUCCESS) {
                        pr_error("[PROC] clone: Failed to copy vspace: %d\n",
                                 (int)e);
                        return -LINUX_ENOMEM;
                }
        }

        /* Create child task structure */
        child = new_task_structure(percpu(kallocator),
                                   &linux_task_append_hooks);
        if (!child) {
                pr_error(
                        "[PROC] clone: Failed to create child task structure\n");
                ret = -LINUX_ENOMEM;
                goto out_put_vspace;
        }

        child->vs = child_vs;
        child->pid = get_new_id(&pid_manager);

        /* Initialize child proc append */
        linux_proc_append_t *parent_pa = linux_proc_append(parent);
        linux_proc_append_t *child_pa = linux_proc_append(child);
        if (child_pa) {
                memset(child_pa, 0, sizeof(*child_pa));
                INIT_LIST_HEAD(&child_pa->pending_exits);
                if (parent_pa) {
                        if (flags & CLONE_VM) {
                                /* Shared address space: share brk and mmap_hint
                                 */
                                child_pa->start_brk = parent_pa->start_brk;
                                child_pa->brk = parent_pa->brk;
                                child_pa->mmap_hint = parent_pa->mmap_hint;
                        } else {
                                /* Separate address space: copy brk, reset hint
                                 */
                                child_pa->start_brk = parent_pa->brk;
                                child_pa->brk = parent_pa->brk;
                                child_pa->mmap_hint = 0;
                        }
                        child_pa->ppid = parent->pid;
                        child_pa->pgid = parent_pa->pgid ? parent_pa->pgid :
                                                           parent->pid;
                        child_pa->uid = parent_pa->uid;
                        child_pa->gid = parent_pa->gid;
                        child_pa->euid = parent_pa->euid;
                        child_pa->egid = parent_pa->egid;
                }
                if (linux_task_append_clone(child, parent, flags)
                    != REND_SUCCESS) {
                        ret = -LINUX_ENOMEM;
                        goto out_free_task;
                }
        }

        /* Add child to task manager */
        e = add_task_to_manager(percpu(core_tm), child);
        if (e != REND_SUCCESS) {
                pr_error(
                        "[PROC] clone: Failed to add child task to task manager: %d\n",
                        (int)e);
                ret = -LINUX_EAGAIN;
                goto out_free_task;
        }

        /*
         * Create child thread.
         *
         * Important: We need to pass the user-provided stack pointer to the
         * child. The stack parameter points to the TOP of the stack (stacks
         * grow downward).
         */
        child_thread =
                copy_thread(parent_thread, child, 0);
        if (!child_thread) {
                pr_error("[PROC] clone: Failed to create child thread\n");
                ret = -LINUX_ENOMEM;
                goto out_del_from_manager;
        }

        if ((flags & CLONE_CHILD_CLEARTID) && child_tid != 0) {
                linux_thread_append_t *child_ta =
                        linux_thread_append(child_thread);

                if (child_ta) {
                        child_ta->clear_tid = child_tid;
                }
        }

        /*
         * __clone child entry pops fn/arg from the supplied stack and needs
         * the hardware user SP (SP_EL0 / user_rsp). copy_thread already
         * arch_ctx_refresh()s the parent ctx and merges it into the child;
         * override only when clone() passed a new stack top.
         */
        if (stack != 0) {
                arch_set_thread_user_sp(&child_thread->ctx, stack);
        }

        if (flags & CLONE_SETTLS) {
                arch_set_user_tls_base(&child_thread->ctx, tls);
        }

        e = add_thread_to_manager(percpu(core_tm), child_thread);
        if (e != REND_SUCCESS) {
                pr_error(
                        "[PROC] clone: Failed to add child thread to scheduler: %d\n",
                        (int)e);
                ret = -LINUX_EAGAIN;
                goto out_free_thread;
        }

        e = register_process(child);
        if (e != REND_SUCCESS) {
                pr_warn("[PROC] clone: Failed to register child PID: %d\n",
                        (int)e);
        }

        if ((flags & CLONE_PARENT_SETTID) && parent_tid != 0 && parent->vs
            && linux_vspace_is_user_table(parent->vs)) {
                tid_t ctid = child_thread->tid;

                if (linux_mm_store_to_user(
                            parent->vs, parent_tid, &ctid, sizeof(ctid))
                    != REND_SUCCESS) {
                        ret = -LINUX_EFAULT;
                        goto out_free_thread;
                }
        }

        if ((flags & CLONE_CHILD_SETTID) && child_tid != 0 && child->vs
            && linux_vspace_is_user_table(child->vs)) {
                tid_t ctid = child_thread->tid;

                if (linux_mm_store_to_user(
                            child->vs, child_tid, &ctid, sizeof(ctid))
                    != REND_SUCCESS) {
                        ret = -LINUX_EFAULT;
                        goto out_free_thread;
                }
        }

        /* TODO: Implement CLONE_FS, CLONE_FILES, CLONE_SIGHAND in Phase 2B/2C
         */

        /*
         * Linux clone(2): parent gets child TID for CLONE_THREAD threads,
         * child PID for fork-style (separate thread group / address space).
         */
        if (flags & CLONE_THREAD) {
                return (i64)child_thread->tid;
        }
        return (i64)child->pid;

out_free_thread:
        if (child_thread) {
                delete_thread(child_thread);
        }
out_del_from_manager:
        del_task_from_manager(child);
out_free_task:
        if (child) {
                delete_task(child);
        }
out_put_vspace:
        /* ref_get (CLONE_VM) or copy failed before child task was created. */
        if (child_vs && child_vs != &root_vspace && !child) {
                ref_put(&child_vs->refcount, free_vspace_ref);
        }
        return ret;
}
