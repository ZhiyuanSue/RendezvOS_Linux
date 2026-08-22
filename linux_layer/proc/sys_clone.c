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
#include <rendezvos/task/thread.h>
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
 * Raw syscall signatures:
 *   x86_64:  clone(flags, stack, parent_tid, child_tid, tls)
 *   aarch64: clone(flags, stack, parent_tid, tls, child_tid)
 * sys_clone() always takes (flags, stack, parent_tid, child_tid, tls);
 * syscall_entry remaps aarch64 arg4/arg5 accordingly.
 *
 * Implementation notes:
 * - New process: linux_proc_alloc + linux_proc_clone_from, then attach.
 *   CLONE_THREAD attaches to the parent proc instead.
 * - Reuses copy_thread() from core (thread.append_hooks.copy)
 * - copy_thread() takes ownership of the VSpace argument onto the child.
 *   CLONE_VM: ref_get parent_vs first, then pass that extra ref.
 *   New AS: pass the cloned vs; do not ref_put after copy_thread.
 * - With CLONE_THREAD: same linux_proc (thread group)
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
        linux_proc_resource_t *parent;
        linux_proc_resource_t *child = NULL;
        Thread_Base *parent_thread;
        Thread_Base *child_thread = NULL;
        VSpace *parent_vs;
        VSpace *child_vs = NULL;
        bool vs_held = false;
        i64 ret;
        error_t e;

        parent_thread = get_cpu_current_thread();
        parent = linux_proc_of(parent_thread);
        parent_vs = parent_thread ? parent_thread->vs : NULL;
        if (!parent || !parent_vs) {
                pr_error("[PROC] clone: Invalid parent task\n");
                return -LINUX_ESRCH;
        }

        ret = validate_clone_flags(flags);
        if (ret != 0) {
                return ret;
        }

        if ((flags & CLONE_VM) && stack == 0) {
                return -LINUX_EINVAL;
        }

        if (flags & CLONE_VM) {
                child_vs = parent_vs;
                if (child_vs && child_vs != &root_vspace) {
                        if (!ref_get_not_zero(&child_vs->refcount))
                                return -LINUX_ENOMEM;
                        vs_held = true;
                }
        } else {
                e = linux_copy_vspace(parent_vs, &child_vs);
                if (e != REND_SUCCESS) {
                        pr_error("[PROC] clone: Failed to copy vspace: %d\n",
                                 (int)e);
                        return -LINUX_ENOMEM;
                }
                vs_held = true;
        }

        if (flags & CLONE_THREAD) {
                child = parent;
        } else {
                child = linux_proc_alloc();
                if (!child) {
                        ret = -LINUX_ENOMEM;
                        goto out_put_vspace;
                }
                INIT_LIST_HEAD(&child->pending_exits);
                if (flags & CLONE_VM) {
                        child->start_brk = parent->start_brk;
                        child->brk = parent->brk;
                        child->mmap_hint = parent->mmap_hint;
                } else {
                        child->start_brk = parent->brk;
                        child->brk = parent->brk;
                        child->mmap_hint = 0;
                }
                child->ppid = parent->pid;
                child->pgid = parent->pgid ? parent->pgid : parent->pid;
                child->uid = parent->uid;
                child->gid = parent->gid;
                child->euid = parent->euid;
                child->egid = parent->egid;
                if (linux_proc_clone_from(child, parent, flags)
                    != REND_SUCCESS) {
                        ret = -LINUX_ENOMEM;
                        goto out_free_proc;
                }
        }

        child_thread = copy_thread(parent_thread, child_vs, 0);
        vs_held = false;
        if (!child_thread) {
                pr_error("[PROC] clone: Failed to create child thread\n");
                ret = -LINUX_ENOMEM;
                goto out_free_proc;
        }

        if (linux_proc_attach_thread(child, child_thread) != REND_SUCCESS) {
                ret = -LINUX_ENOMEM;
                goto out_free_thread;
        }

        if ((flags & CLONE_CHILD_CLEARTID) && child_tid != 0) {
                linux_thread_append_t *child_ta =
                        linux_thread_append(child_thread);

                if (child_ta) {
                        child_ta->clear_tid = child_tid;
                }
        }

        if (stack != 0) {
                arch_set_thread_user_sp(&child_thread->ctx, stack);
        }

        if (flags & CLONE_SETTLS) {
                arch_set_user_tls_base(&child_thread->ctx, tls);
        }

        if (!(flags & CLONE_VM) && parent_vs) {
                linux_mm_cow_break_user_stack(
                        parent_vs,
                        arch_get_thread_user_sp(&parent_thread->ctx));
        }

        e = add_thread_to_manager(percpu(core_tm), child_thread);
        if (e != REND_SUCCESS) {
                pr_error(
                        "[PROC] clone: Failed to add child thread to scheduler: %d\n",
                        (int)e);
                ret = -LINUX_EAGAIN;
                goto out_free_thread;
        }

        if (!(flags & CLONE_THREAD)) {
                e = register_process(child);
                if (e != REND_SUCCESS) {
                        pr_warn("[PROC] clone: Failed to register child PID: %d\n",
                                (int)e);
                }
        }

        if ((flags & CLONE_PARENT_SETTID) && parent_tid != 0 && parent_vs
            && linux_vspace_is_user_table(parent_vs)) {
                tid_t ctid = child_thread->tid;

                if (linux_mm_store_to_user(
                            parent_vs, parent_tid, &ctid, sizeof(ctid))
                    != REND_SUCCESS) {
                        ret = -LINUX_EFAULT;
                        goto out_started;
                }
        }

        if ((flags & CLONE_CHILD_SETTID) && child_tid != 0 && child_vs
            && linux_vspace_is_user_table(child_vs)) {
                tid_t ctid = child_thread->tid;

                if (linux_mm_store_to_user(
                            child_vs, child_tid, &ctid, sizeof(ctid))
                    != REND_SUCCESS) {
                        ret = -LINUX_EFAULT;
                        goto out_started;
                }
        }

        if (flags & CLONE_THREAD) {
                return (i64)child_thread->tid;
        }
        return (i64)child->pid;

out_started:
        /* Thread already runnable; vs already on the child. */
        return ret;
out_free_thread:
        delete_thread(child_thread);
out_free_proc:
        if (child && child != parent)
                (void)linux_proc_put(child);
out_put_vspace:
        if (vs_held && child_vs && child_vs != &root_vspace)
                (void)ref_put(&child_vs->refcount, free_vspace_ref);
        return ret;
}
