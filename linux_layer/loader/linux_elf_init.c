#include <linux_compat/append_hooks.h>
#include <linux_compat/clone_flags.h>
#include <linux_compat/ipc/clean_protocol.h>
#include <linux_compat/proc/linux_exec.h>
#include <linux_compat/proc/linux_exec_proc.h>

#include <common/align.h>
#include <common/stddef.h>
#include <common/string.h>
#include <common/types.h>
#include <common/dsa/list.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/ipc/rpc.h>
#include <linux_compat/proc/wait_ipc.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/signal/signal_state.h>
#include <linux_compat/time/linux_time_sleep.h>
#include <linux_compat/initcall.h>
#include <modules/log/log.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/mm/page_slice.h>

extern struct Port_Table *global_port_table;

const task_append_hooks_t linux_task_append_hooks = {
        .append_info_len = LINUX_PROC_APPEND_BYTES,
        .copy = linux_task_append_copy,
        .fini = linux_task_append_fini,
};

const thread_append_hooks_t linux_thread_append_hooks = {
        .append_info_len = LINUX_THREAD_APPEND_BYTES,
        .init = linux_thread_append_init,
        .copy = linux_thread_append_copy,
        .fini = linux_thread_append_fini,
};

void linux_task_append_fini(Tcb_Base *tcb)
{
        Tcb_Base *task = tcb;
        linux_proc_append_t *pa;
        pid_t pid;

        if (!task)
                return;

        pa = linux_proc_append(task);
        if (pa)
                linux_proc_wait_pending_drain(pa);

        pid = task->pid;
        proc_reparent_children(pid, LINUX_INIT_REAP_PPID);
        proc_unregister_wait_port(pid);
        ipc_rpc_unregister_port_by_pid(VFS_CLIENT_PORT_PREFIX, pid);
        ipc_rpc_unregister_port_by_pid(CLEAN_CLIENT_PORT_PREFIX, pid);
        unregister_process(task);
        linux_signal_proc_destroy(task);
        linux_fs_proc_destroy(task);
}

error_t linux_task_append_copy(Tcb_Base *dst, Tcb_Base *src)
{
        Tcb_Base *d = dst;
        Tcb_Base *s = src;
        linux_proc_append_t *spa;

        if (!d) {
                return -E_IN_PARAM;
        }

        spa = s ? linux_proc_append(s) : NULL;
        if (spa) {
                if (linux_signal_proc_fork(d, s) != REND_SUCCESS) {
                        return -E_RENDEZVOS;
                }
                if (linux_fs_proc_fork(d, s) != REND_SUCCESS) {
                        return -E_RENDEZVOS;
                }
        } else {
                if (linux_signal_proc_attach(d) != REND_SUCCESS) {
                        return -E_RENDEZVOS;
                }
                if (linux_fs_proc_attach(d) != REND_SUCCESS) {
                        return -E_RENDEZVOS;
                }
        }

        return REND_SUCCESS;
}

error_t linux_task_append_clone(Tcb_Base *dst, Tcb_Base *src, u64 clone_flags)
{
        Tcb_Base *d = dst;
        Tcb_Base *s = src;

        if (!d) {
                return -E_IN_PARAM;
        }
        if (!s) {
                return linux_task_append_copy(dst, NULL);
        }
        if (clone_flags & CLONE_VM) {
                if (linux_signal_proc_attach(d) != REND_SUCCESS) {
                        return -E_RENDEZVOS;
                }
        } else if (linux_signal_proc_fork(d, s) != REND_SUCCESS) {
                return -E_RENDEZVOS;
        }
        if (linux_fs_proc_fork(d, s) != REND_SUCCESS) {
                return -E_RENDEZVOS;
        }
        return REND_SUCCESS;
}

void linux_thread_append_fini(Thread_Base *thread)
{
        Thread_Base *thr = thread;
        char kport[PORT_NAME_LEN_MAX];
        const char *pfx = "vfs_cli_k_t";
        size_t plen;
        size_t i;

        if (!thr) {
                return;
        }

        /*
         * Backend RPC reply ports are vfs_cli_k_t<tid>. Drop on thread
         * teardown so the global name table does not grow without bound
         * across run_all forks (and a reused tid cannot hit a stale port).
         */
        plen = strlen(pfx);
        if (plen < sizeof(kport)) {
                memcpy(kport, pfx, plen);
                i = plen;
                if (proc_format_pid(
                            kport + i, sizeof(kport) - i, (pid_t)thr->tid)
                    != 0) {
                        ipc_rpc_unregister_port_name(kport);
                }
        }

        linux_time_sleep_port_teardown(thr);
        linux_signal_thread_destroy(thr);
}

error_t linux_thread_append_copy(Thread_Base *dst, Thread_Base *src)
{
        Thread_Base *d = dst;
        Thread_Base *s = src;
        linux_thread_append_t *dst_ta;

        if (!d) {
                return -E_IN_PARAM;
        }

        dst_ta = linux_thread_append(d);
        if (!dst_ta) {
                return REND_SUCCESS;
        }

        /* Fork/clone policy: child must not inherit runner cookie or clear_tid.
         */
        dst_ta->boot_wait_cookie = 0;
        dst_ta->clear_tid = 0;

        return linux_signal_thread_fork_inherit(d, s, true);
}

error_t linux_thread_append_init(Thread_Base *thread,
                                 const elf_load_info_t *info)
{
        Thread_Base *thr = thread;
        Tcb_Base *tcb = thr ? thr->belong_tcb : NULL;
        linux_proc_append_t *pa;

        if (!info) {
                return -E_IN_PARAM;
        }
        if (!thr || !(thr->flags & THREAD_FLAG_USER)) {
                pr_emer("[LINUX_ELF_INIT] ERROR: not a user thread (thr=%p)\n",
                        (void *)thr);
                return -E_IN_PARAM;
        }

        pa = linux_proc_append(tcb);
        if (!tcb || !pa) {
                pr_emer("[LINUX_ELF_INIT] ERROR: missing belong_tcb/pa (tcb=%p)\n",
                        (void *)tcb);
                return -E_IN_PARAM;
        }

        /*
         * append.init for gen_task_from_elf / run_elf_program only.
         * Linux user images must use linux_exec_replace_image (PID1 / sys_execve);
         * this hook must not build argv/auxv (that was the old Path B bootstrap).
         */
        if (linux_user_task_prepare_new(tcb, thr) != REND_SUCCESS) {
                pr_emer("[LINUX_ELF_INIT] ERROR: prepare_new failed pid=%d\n",
                        tcb->pid);
                return -E_RENDEZVOS;
        }
        linux_proc_set_heap_from_elf_load(tcb, info->max_load_end);

        if (info->slice) {
                struct page_slice *s = info->slice;

                page_slice_destroy(&s);
        }

        return REND_SUCCESS;
}

static bool linux_elf_init_logged;

static void linux_elf_init_initcall(void)
{
        if (!linux_init_bsp_once(&linux_elf_init_logged))
                return;
        pr_info("[LINUX_ELF_INIT] Module initialized\n");
        linux_init_bsp_mark_done(&linux_elf_init_logged);
}
DEFINE_INIT(linux_elf_init_initcall);
