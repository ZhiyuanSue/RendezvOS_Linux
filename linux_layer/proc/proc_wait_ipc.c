/*
 * wait4 port wake: child exit notify + signal EINTR interrupt (see sys_wait.c).
 */

#include <linux_compat/ipc/exit_protocol.h>
#include <linux_compat/proc/clean_ipc.h>
#include <linux_compat/proc/wait_ipc.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/signal/signal_deliver.h>
#include <modules/log/log.h>
#include <common/dsa/list.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/sync/cas_lock.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>

typedef struct linux_wait_pending_exit {
        struct list_entry node;
        pid_t pid;
        i32 exit_code;
} linux_wait_pending_exit_t;

static error_t linux_proc_wait_deliver_message(Message_t *msg,
                                               Message_Port_t *port)
{
        error_t err;

        err = enqueue_msg_for_send(msg);
        if (err != REND_SUCCESS) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                ref_put(&port->refcount, free_message_port_ref);
                pr_error("[PROC/wait_ipc] enqueue_msg_for_send failed e=%d\n",
                         (int)err);
                return err;
        }

        err = send_msg(port);
        if (err != REND_SUCCESS)
                pr_error("[PROC/wait_ipc] send_msg failed e=%d\n", (int)err);
        ref_put(&port->refcount, free_message_port_ref);
        return err;
}

static bool wait_pending_pid_matches(i32 want_pid, pid_t child_pid,
                                     Tcb_Base *parent,
                                     linux_proc_append_t *parent_pa)
{
        Tcb_Base *child;
        linux_proc_append_t *child_pa;

        if (want_pid == -1)
                return true;
        if (want_pid > 0)
                return child_pid == (pid_t)want_pid;

        child = find_task_by_pid(child_pid);
        if (!child)
                return false;
        child_pa = linux_proc_append(child);
        if (!child_pa || child_pa->ppid != parent->pid)
                return false;
        if (want_pid == 0) {
                if (!parent_pa)
                        return false;
                return child_pa->pgid == parent_pa->pgid;
        }
        return child_pa->pgid == (pid_t)(-want_pid);
}

bool linux_proc_wait_pending_push(linux_proc_append_t *parent_pa, pid_t pid,
                                  i32 exit_code)
{
        struct allocator *alloc;
        linux_wait_pending_exit_t *ent;

        if (!parent_pa || pid <= 0)
                return false;

        alloc = percpu(kallocator);
        if (!alloc || !alloc->m_alloc)
                return false;

        ent = (linux_wait_pending_exit_t *)alloc->m_alloc(alloc, sizeof(*ent));
        if (!ent)
                return false;

        INIT_LIST_HEAD(&ent->node);
        ent->pid = pid;
        ent->exit_code = exit_code;
        list_add_tail(&ent->node, &parent_pa->pending_exits);
        return true;
}

bool linux_proc_wait_pending_take(linux_proc_append_t *parent_pa, i32 want_pid,
                                  Tcb_Base *parent, pid_t *pid_out,
                                  i32 *exit_code_out)
{
        struct list_entry *pos;
        struct list_entry *n;
        struct allocator *alloc;

        if (!parent_pa || !parent || !pid_out || !exit_code_out)
                return false;
        if (!list_node_is_valid(&parent_pa->pending_exits)
            || list_empty(&parent_pa->pending_exits))
                return false;

        alloc = percpu(kallocator);
        list_for_each_safe(pos, n, &parent_pa->pending_exits) {
                linux_wait_pending_exit_t *ent =
                        list_entry(pos, linux_wait_pending_exit_t, node);

                if (!wait_pending_pid_matches(want_pid, ent->pid, parent,
                                              parent_pa))
                        continue;

                *pid_out = ent->pid;
                *exit_code_out = ent->exit_code;
                list_del_init(&ent->node);
                if (alloc && alloc->m_free)
                        alloc->m_free(alloc, ent);
                return true;
        }
        return false;
}

void linux_proc_wait_pending_drain(linux_proc_append_t *parent_pa)
{
        struct list_entry *pos;
        struct list_entry *n;
        struct allocator *alloc;

        if (!parent_pa)
                return;
        if (!list_node_is_valid(&parent_pa->pending_exits))
                return;

        alloc = percpu(kallocator);
        list_for_each_safe(pos, n, &parent_pa->pending_exits) {
                linux_wait_pending_exit_t *ent =
                        list_entry(pos, linux_wait_pending_exit_t, node);

                list_del_init(&ent->node);
                if (alloc && alloc->m_free)
                        alloc->m_free(alloc, ent);
        }
        INIT_LIST_HEAD(&parent_pa->pending_exits);
}

static bool linux_proc_wait_post_interrupt(Message_Port_t *port)
{
        Msg_Data_t *md;
        Message_t *msg;
        error_t err;

        if (!port)
                return false;

        md = kmsg_create(port->service_id,
                         KMSG_OP_PROC_WAIT_INTERRUPT,
                         LINUX_KMSG_FMT_WAIT_INTERRUPT,
                         (i64)0);
        if (!md)
                return false;

        msg = create_message_with_msg(md);
        ref_put(&md->refcount, free_msgdata_ref_default);
        if (!msg)
                return false;

        err = ipc_system_try_deliver(port, msg, false);
        return err == REND_SUCCESS;
}

void linux_proc_wait_wake_for_signal(Thread_Base *thread, Tcb_Base *process)
{
        Message_Port_t *wait_port;

        if (!thread || !process)
                return;
        if (thread_get_status(thread) != thread_status_block_on_receive)
                return;
        if (!linux_signal_wait4_should_return_eintr(thread))
                return;

        wait_port = proc_get_or_create_wait_port(process->pid);
        if (!wait_port)
                return;
        if ((Message_Port_t *)thread->port_ptr != wait_port)
                return;

        (void)linux_proc_wait_post_interrupt(wait_port);
}

bool linux_proc_post_exit_notify(pid_t parent_pid, pid_t child_pid,
                                 i32 exit_code)
{
        Message_Port_t *wait_port;
        Msg_Data_t *md;
        Message_t *msg;
        error_t err;

        if (parent_pid <= 0 || child_pid <= 0)
                return false;

        wait_port = proc_get_or_create_wait_port(parent_pid);
        if (!wait_port)
                return false;

        md = kmsg_create(wait_port->service_id,
                         KMSG_OP_PROC_EXIT_NOTIFY,
                         LINUX_KMSG_FMT_EXIT_NOTIFY,
                         (i64)child_pid,
                         exit_code);
        if (!md) {
                ref_put(&wait_port->refcount, free_message_port_ref);
                return false;
        }

        msg = create_message_with_msg(md);
        ref_put(&md->refcount, free_msgdata_ref_default);
        if (!msg) {
                ref_put(&wait_port->refcount, free_message_port_ref);
                return false;
        }

        /*
         * Blocking send is OK: clean_server runs this from a per-message
         * worker thread, so the listen loop stays available for other
         * THREAD_REAP / TASK_REAP messages.
         */
        err = linux_proc_wait_deliver_message(msg, wait_port);
        return err == REND_SUCCESS;
}

bool linux_proc_post_kernel_exit_notify(pid_t child_pid, i32 exit_code)
{
        Message_Port_t *kernel_port;
        Msg_Data_t *md;
        Message_t *msg;
        error_t err;

        if (child_pid <= 0)
                return false;

        kernel_port = thread_lookup_port(KERNEL_PORT_NAME);
        if (!kernel_port) {
                pr_error("[PROC/wait_ipc] kernel port '%s' not found\n",
                         KERNEL_PORT_NAME);
                return false;
        }

        md = kmsg_create(kernel_port->service_id,
                         KMSG_OP_PROC_EXIT_NOTIFY,
                         LINUX_KMSG_FMT_EXIT_NOTIFY,
                         (i64)child_pid,
                         exit_code);
        if (!md) {
                ref_put(&kernel_port->refcount, free_message_port_ref);
                return false;
        }

        msg = create_message_with_msg(md);
        ref_put(&md->refcount, free_msgdata_ref_default);
        if (!msg) {
                ref_put(&kernel_port->refcount, free_message_port_ref);
                return false;
        }

        err = linux_proc_wait_deliver_message(msg, kernel_port);
        return err == REND_SUCCESS;
}

bool linux_proc_reap_zombie_by_pid(pid_t child_pid)
{
        Tcb_Base *child;
        linux_proc_append_t *pa;
        bool task_empty;

        if (child_pid <= 0)
                return false;

        child = find_task_by_pid(child_pid);
        if (!child)
                return false;

        pa = linux_proc_append(child);
        if (!pa || pa->exit_state != 1)
                return false;

        lock_cas(&child->thread_list_lock);
        task_empty = (child->thread_number == 0);
        if (!task_empty) {
                unlock_cas(&child->thread_list_lock);
                return false;
        }
        pa->exit_state = 2;
        unlock_cas(&child->thread_list_lock);

        (void)linux_clean_send_task_reap(child_pid);
        return true;
}
