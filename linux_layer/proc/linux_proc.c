#include <linux_compat/append_hooks.h>
#include <linux_compat/clone_flags.h>
#include <linux_compat/initcall.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/ipc/clean_protocol.h>
#include <linux_compat/ipc/rpc.h>
#include <linux_compat/proc/wait_ipc.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/signal/signal_state.h>
#include <linux_compat/time/linux_time_sleep.h>

#include <common/dsa/list.h>
#include <common/stddef.h>
#include <common/string.h>
#include <modules/log/log.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/sync/cas_lock.h>
#include <rendezvos/task/id.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/task/thread.h>

static Id_Manager linux_pid_manager;
static bool linux_pid_id_inited;

static void linux_pid_id_init(void)
{
        if (!linux_init_bsp_once(&linux_pid_id_inited))
                return;
        init_id_manager(&linux_pid_manager);
        linux_init_bsp_mark_done(&linux_pid_id_inited);
}

DEFINE_INIT(linux_pid_id_init);

static error_t linux_proc_free_ref(ref_count_t *ref)
{
        linux_proc_resource_t *proc;
        struct allocator *alloc;

        if (!ref)
                return -E_IN_PARAM;
        proc = container_of(ref, linux_proc_resource_t, refcount);
        alloc = percpu(kallocator);
        if (alloc)
                alloc->m_free(alloc, proc);
        return REND_SUCCESS;
}

linux_proc_resource_t *linux_proc_alloc(void)
{
        struct allocator *alloc = percpu(kallocator);
        linux_proc_resource_t *proc;

        if (!alloc)
                return NULL;
        proc = (linux_proc_resource_t *)alloc->m_alloc(alloc, sizeof(*proc));
        if (!proc)
                return NULL;
        memset(proc, 0, sizeof(*proc));
        ref_init(&proc->refcount);
        /*
         * linux_pid_manager starts at 0. Linux user pids must be > 0: 0 is
         * LINUX_INIT_REAP_PPID / wait_port and vfs_cli reject pid<=0.
         */
        do {
                proc->pid = get_new_id(&linux_pid_manager);
        } while (proc->pid == 0);
        if (proc->pid == INVALID_ID) {
                alloc->m_free(alloc, proc);
                return NULL;
        }
        lock_init_cas(&proc->thread_list_lock);
        INIT_LIST_HEAD(&proc->thread_head_node);
        INIT_LIST_HEAD(&proc->pending_exits);
        proc->exit_state = LINUX_EXIT_RUNNING;
        return proc;
}

error_t linux_proc_attach_thread(linux_proc_resource_t *proc, Thread_Base *thread)
{
        linux_thread_append_t *ta;

        if (!proc || !thread)
                return -E_IN_PARAM;
        ta = linux_thread_append(thread);
        if (!ta)
                return -E_IN_PARAM;
        if (ta->res && ta->res != proc)
                return -E_RENDEZVOS;
        if (ta->res == proc)
                return REND_SUCCESS;
        if (!ref_get_not_zero(&proc->refcount))
                return -E_RENDEZVOS;
        lock_cas(&proc->thread_list_lock);
        INIT_LIST_HEAD(&ta->res_thread_node);
        list_add_tail(&ta->res_thread_node, &proc->thread_head_node);
        proc->thread_number++;
        ta->res = proc;
        if (!proc->vs)
                proc->vs = thread->vs;
        unlock_cas(&proc->thread_list_lock);
        return REND_SUCCESS;
}

void linux_proc_detach_thread(Thread_Base *thread)
{
        linux_thread_append_t *ta;
        linux_proc_resource_t *proc;

        if (!thread)
                return;
        ta = linux_thread_append(thread);
        if (!ta)
                return;
        proc = ta->res;
        if (!proc)
                return;
        lock_cas(&proc->thread_list_lock);
        list_del_init(&ta->res_thread_node);
        if (proc->thread_number > 0)
                proc->thread_number--;
        ta->res = NULL;
        /*
         * Last thread: drop the non-owning cache while thread->vs is still
         * live (core calls fini before the thread->vs put). linux_proc never owns vs.
         */
        if (proc->thread_number == 0)
                proc->vs = NULL;
        unlock_cas(&proc->thread_list_lock);
        (void)linux_proc_put(proc);
}

void linux_proc_wait_all_threads_detached(linux_proc_resource_t *proc)
{
        if (!proc)
                return;
        for (;;) {
                lock_cas(&proc->thread_list_lock);
                if (proc->thread_number == 0) {
                        unlock_cas(&proc->thread_list_lock);
                        return;
                }
                unlock_cas(&proc->thread_list_lock);
                schedule(percpu(core_tm));
        }
}

void linux_proc_fini(linux_proc_resource_t *proc)
{
        pid_t pid;

        if (!proc)
                return;

        linux_proc_wait_pending_drain(proc);
        pid = proc->pid;
        proc_reparent_children(pid, LINUX_INIT_REAP_PPID);
        proc_unregister_wait_port(pid);
        ipc_rpc_unregister_port_by_pid(VFS_CLIENT_PORT_PREFIX, pid);
        unregister_process(proc);
        linux_signal_proc_destroy(proc);
        linux_fs_proc_destroy(proc);
}

error_t linux_proc_put(linux_proc_resource_t *proc)
{
        if (!proc)
                return -E_IN_PARAM;
        return ref_put(&proc->refcount, linux_proc_free_ref);
}

error_t linux_proc_reap(linux_proc_resource_t *proc)
{
        if (!proc)
                return -E_IN_PARAM;
        lock_cas(&proc->thread_list_lock);
        if (proc->thread_number != 0) {
                unlock_cas(&proc->thread_list_lock);
                return -E_RENDEZVOS;
        }
        if (proc->exit_state == LINUX_EXIT_CLAIMED) {
                unlock_cas(&proc->thread_list_lock);
                return -E_RENDEZVOS;
        }
        if (proc->exit_state != LINUX_EXIT_ZOMBIE
            && proc->exit_state != LINUX_EXIT_NOTIFIED) {
                unlock_cas(&proc->thread_list_lock);
                return -E_RENDEZVOS;
        }
        proc->exit_state = LINUX_EXIT_CLAIMED;
        unlock_cas(&proc->thread_list_lock);
        linux_proc_fini(proc);
        return linux_proc_put(proc);
}

error_t linux_proc_copy_from(linux_proc_resource_t *dst, linux_proc_resource_t *src)
{
        if (!dst)
                return -E_IN_PARAM;
        if (src) {
                if (linux_signal_proc_fork(dst, src) != REND_SUCCESS)
                        return -E_RENDEZVOS;
                if (linux_fs_proc_fork(dst, src) != REND_SUCCESS)
                        return -E_RENDEZVOS;
        } else {
                if (linux_signal_proc_attach(dst) != REND_SUCCESS)
                        return -E_RENDEZVOS;
                if (linux_fs_proc_attach(dst) != REND_SUCCESS)
                        return -E_RENDEZVOS;
        }
        return REND_SUCCESS;
}

error_t linux_proc_clone_from(linux_proc_resource_t *dst, linux_proc_resource_t *src,
                              u64 clone_flags)
{
        if (!dst)
                return -E_IN_PARAM;
        if (!src)
                return linux_proc_copy_from(dst, NULL);
        if (clone_flags & CLONE_VM) {
                if (linux_signal_proc_attach(dst) != REND_SUCCESS)
                        return -E_RENDEZVOS;
        } else if (linux_signal_proc_fork(dst, src) != REND_SUCCESS) {
                return -E_RENDEZVOS;
        }
        if (linux_fs_proc_fork(dst, src) != REND_SUCCESS)
                return -E_RENDEZVOS;
        return REND_SUCCESS;
}
