#include <common/string.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/append_hooks.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/vspace_copy.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/sync/cas_lock.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/task/thread.h>
#include <syscall.h>
#if defined(_X86_64_)
#include <arch/x86_64/thread_arch.h>
#elif defined(_AARCH64_)
#include <arch/aarch64/thread_arch.h>
#endif

i64 sys_fork(void)
{
        linux_proc_resource_t *parent;
        linux_proc_resource_t *child = NULL;
        VSpace *parent_vs;
        VSpace *child_vs = NULL;
        Thread_Base *parent_thread;
        Thread_Base *child_thread = NULL;
        i64 ret = -LINUX_ENOMEM;
        error_t e;

        parent_thread = get_cpu_current_thread();
        parent = linux_proc_of(parent_thread);
        parent_vs = parent_thread ? parent_thread->vs : NULL;
        if (!parent || !parent_vs) {
                pr_error("[PROC] fork: Invalid parent task\n");
                return -LINUX_ESRCH;
        }

        if (!linux_vspace_is_user_table(parent_vs)) {
                pr_error(
                        "[PROC] fork: Parent has no user vspace (radix/page tables)\n");
                return -LINUX_EINVAL;
        }

        child = linux_proc_alloc();
        if (!child) {
                pr_error("[PROC] fork: Failed to create child proc\n");
                return -LINUX_ENOMEM;
        }

        e = linux_copy_vspace(parent_vs, &child_vs);
        if (e != REND_SUCCESS) {
                pr_error("[PROC] fork: Failed to copy vspace: %d\n", (int)e);
                ret = -LINUX_ENOMEM;
                goto out_free_proc;
        }

        child->ppid = parent->pid;
        child->exit_code = 0;
        child->exit_state = LINUX_EXIT_RUNNING;
        child->start_brk = parent->brk;
        child->brk = parent->brk;
        child->mmap_hint = parent->mmap_hint;
        child->pgid = parent->pgid ? parent->pgid : parent->pid;
        child->uid = parent->uid;
        child->gid = parent->gid;
        child->euid = parent->euid;
        child->egid = parent->egid;
        if (linux_proc_copy_from(child, parent) != REND_SUCCESS) {
                ret = -LINUX_ENOMEM;
                goto out_free_vspace;
        }

        child_thread = copy_thread(parent_thread, child_vs, 0);
        if (!child_thread) {
                pr_error("[PROC] fork: Failed to create child thread\n");
                ret = -LINUX_ENOMEM;
                goto out_free_proc;
        }

        if (linux_proc_attach_thread(child, child_thread) != REND_SUCCESS) {
                ret = -LINUX_ENOMEM;
                goto out_free_thread;
        }

        e = add_thread_to_manager(percpu(core_tm), child_thread);
        if (e != REND_SUCCESS) {
                pr_error("[PROC] fork: Failed to start child thread: %d\n",
                         (int)e);
                ret = -LINUX_EAGAIN;
                goto out_free_thread;
        }

        e = register_process(child);
        if (e != REND_SUCCESS) {
                pr_warn("[PROC] fork: Failed to register child PID: %d\n",
                        (int)e);
        }

        linux_mm_cow_break_user_stack(
                parent_vs, arch_get_thread_user_sp(&parent_thread->ctx));

        return (i64)child->pid;

out_free_thread:
        delete_thread(child_thread);
        child_thread = NULL;
        goto out_free_proc;
out_free_vspace:
        if (child_vs && child_vs != &root_vspace)
                (void)ref_put(&child_vs->refcount, free_vspace_ref);
out_free_proc:
        (void)linux_proc_put(child);
        return ret;
}
