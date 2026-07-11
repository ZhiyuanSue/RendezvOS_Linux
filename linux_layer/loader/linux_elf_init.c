#include <linux_compat/elf_init.h>
#include <linux_compat/append_fini.h>

#include <common/align.h>
#include <common/stddef.h>
#include <common/types.h>
#include <linux_compat/proc/linux_exec_proc.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/ipc/rpc.h>
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

elf_init_handler_t linux_elf_init_handler_ptr = linux_elf_init_handler;
task_append_fini_t linux_task_append_fini_ptr = linux_task_append_fini;
task_append_copy_t linux_task_append_fork_ptr = linux_task_append_fork;
thread_append_fini_t linux_thread_append_fini_ptr = linux_thread_append_fini;
thread_append_copy_t linux_thread_append_fork_ptr = linux_thread_append_fork;

void linux_task_append_fini(struct Tcb_Base *tcb)
{
        Tcb_Base *task = (Tcb_Base *)tcb;
        pid_t pid;

        if (!task) {
                return;
        }

        pid = task->pid;
        proc_reparent_children(pid, LINUX_INIT_REAP_PPID);
        proc_unregister_wait_port(pid);
        ipc_rpc_unregister_port_by_pid(VFS_CLIENT_PORT_PREFIX, pid);
        unregister_process(task);
        linux_signal_proc_destroy(task);
        linux_fs_proc_destroy(task);
}

error_t linux_task_append_fork(struct Tcb_Base *child, struct Tcb_Base *parent)
{
        Tcb_Base *c = (Tcb_Base *)child;
        Tcb_Base *p = (Tcb_Base *)parent;
        linux_proc_append_t *ppa;

        if (!c) {
                return -E_IN_PARAM;
        }

        ppa = p ? linux_proc_append(p) : NULL;
        if (ppa) {
                if (linux_signal_proc_fork(c, p) != REND_SUCCESS) {
                        return -E_RENDEZVOS;
                }
                if (linux_fs_proc_fork(c, p) != REND_SUCCESS) {
                        return -E_RENDEZVOS;
                }
        } else {
                if (linux_signal_proc_attach(c) != REND_SUCCESS) {
                        return -E_RENDEZVOS;
                }
                if (linux_fs_proc_attach(c) != REND_SUCCESS) {
                        return -E_RENDEZVOS;
                }
        }

        return REND_SUCCESS;
}

void linux_thread_append_fini(struct Thread_Base *thread)
{
        Thread_Base *thr = (Thread_Base *)thread;

        if (!thr) {
                return;
        }

        linux_time_sleep_port_teardown(thr);
        linux_signal_thread_destroy(thr);
}

error_t linux_thread_append_fork(struct Thread_Base *child,
                                 struct Thread_Base *parent)
{
        Thread_Base *c = (Thread_Base *)child;
        Thread_Base *p = (Thread_Base *)parent;
        linux_thread_append_t *child_ta;

        if (!c) {
                return -E_IN_PARAM;
        }

        child_ta = linux_thread_append(c);
        if (!child_ta) {
                return REND_SUCCESS;
        }

        child_ta->test_cookie = 0;
        child_ta->clear_tid = 0;

        return linux_signal_thread_fork_inherit(c, p, true);
}

void *linux_elf_init_handler(Arch_Task_Context *ctx,
                             const elf_load_info_t *info)
{
        (void)ctx;

        if (!info) {
                pr_emer("[LINUX_ELF_INIT] ERROR: info is NULL!\n");
                return NULL;
        }

        Thread_Base *thr = container_of(ctx, Thread_Base, ctx);
        Tcb_Base *tcb = thr ? thr->belong_tcb : NULL;
        linux_proc_append_t *pa = linux_proc_append(tcb);

        if (!thr || !(thr->flags & THREAD_FLAG_USER)) {
                pr_emer("[LINUX_ELF_INIT] ERROR: not a user thread (thr=%p)\n",
                        (void *)thr);
                return NULL;
        }
        if (!tcb || !pa) {
                pr_emer("[LINUX_ELF_INIT] ERROR: missing belong_tcb/pa (tcb=%p)\n",
                        (void *)tcb);
                return NULL;
        }

        linux_proc_set_heap_from_elf_load(tcb, info->max_load_end);
        if (linux_signal_proc_attach(tcb) != REND_SUCCESS) {
                pr_emer("[LINUX_ELF_INIT] ERROR: signal attach failed for pid=%d\n",
                        tcb->pid);
                return NULL;
        }
        if (linux_signal_thread_attach(thr) != REND_SUCCESS) {
                pr_emer("[LINUX_ELF_INIT] ERROR: thread signal attach failed pid=%d\n",
                        tcb->pid);
                return NULL;
        }
        if (linux_fs_proc_attach(tcb) != REND_SUCCESS) {
                pr_emer("[LINUX_ELF_INIT] ERROR: fs attach failed for pid=%d\n",
                        tcb->pid);
                return NULL;
        }

        error_t reg_e = register_process(tcb);
        if (reg_e != REND_SUCCESS) {
                pr_warn("[LINUX_ELF_INIT] Failed to register PID=%d: %d\n",
                        tcb->pid,
                        (int)reg_e);
        }

        Message_Port_t *wait_port = proc_get_or_create_wait_port(tcb->pid);
        if (!wait_port) {
                pr_warn("[LINUX_ELF_INIT] Failed to create wait_port for PID=%d\n",
                        tcb->pid);
        } else {
                ref_put(&wait_port->refcount, free_message_port_ref);
        }

        /*
         * Compat policy: file image is copied into user PT_LOAD; drop the
         * staging slice after load. Future page-cache / LRU may retain it.
         */
        if (info->slice) {
                struct page_slice *s = info->slice;

                page_slice_destroy(&s);
        }

        return NULL;
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
