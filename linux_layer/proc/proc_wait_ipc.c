/*
 * wait4 port wake: EXIT_NOTIFY delivery + signal EINTR interrupt (sys_wait.c).
 */

#include <linux_compat/ipc/exit_protocol.h>
#include <linux_compat/proc/wait_ipc.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/signal/signal_deliver.h>
#include <modules/log/log.h>
#include <common/dsa/list.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/thread.h>

typedef struct linux_wait_pending_exit {
        struct list_entry node;
        linux_proc_resource_t *child;
        pid_t pid;
        i32 exit_code;
} linux_wait_pending_exit_t;

bool linux_proc_wait_pid_matches(i32 want_pid, pid_t child_pid,
                                 linux_proc_resource_t *parent)
{
        linux_proc_resource_t *child;

        if (want_pid == -1)
                return true;
        if (want_pid > 0)
                return child_pid == (pid_t)want_pid;
        if (!parent)
                return false;

        child = find_proc_by_pid(child_pid);
        if (!child || child->ppid != parent->pid)
                return false;
        if (want_pid == 0)
                return child->pgid == parent->pgid;
        return child->pgid == (pid_t)(-want_pid);
}

bool linux_proc_wait_pending_push(linux_proc_resource_t *parent,
                                  linux_proc_resource_t *child, i32 exit_code)
{
        struct allocator *alloc;
        linux_wait_pending_exit_t *ent;

        if (!parent || !child || child->pid <= 0)
                return false;

        alloc = percpu(kallocator);
        if (!alloc || !alloc->m_alloc)
                return false;

        ent = (linux_wait_pending_exit_t *)alloc->m_alloc(alloc, sizeof(*ent));
        if (!ent)
                return false;

        INIT_LIST_HEAD(&ent->node);
        ent->child = child;
        ent->pid = child->pid;
        ent->exit_code = exit_code;
        list_add_tail(&ent->node, &parent->pending_exits);
        return true;
}

bool linux_proc_wait_pending_take(linux_proc_resource_t *parent, i32 want_pid,
                                  linux_proc_resource_t **child_out,
                                  i32 *exit_code_out)
{
        struct list_entry *pos;
        struct list_entry *n;
        struct allocator *alloc;

        if (!parent || !child_out || !exit_code_out)
                return false;
        if (!list_node_is_valid(&parent->pending_exits)
            || list_empty(&parent->pending_exits))
                return false;

        alloc = percpu(kallocator);
        list_for_each_safe(pos, n, &parent->pending_exits)
        {
                linux_wait_pending_exit_t *ent =
                        list_entry(pos, linux_wait_pending_exit_t, node);

                if (!linux_proc_wait_pid_matches(want_pid, ent->pid, parent))
                        continue;

                *child_out = ent->child;
                *exit_code_out = ent->exit_code;
                list_del_init(&ent->node);
                if (alloc && alloc->m_free)
                        alloc->m_free(alloc, ent);
                return true;
        }
        return false;
}

void linux_proc_wait_pending_drain(linux_proc_resource_t *parent)
{
        struct list_entry *pos;
        struct list_entry *n;
        struct allocator *alloc;

        if (!parent)
                return;
        if (!list_node_is_valid(&parent->pending_exits))
                return;

        alloc = percpu(kallocator);
        list_for_each_safe(pos, n, &parent->pending_exits)
        {
                linux_wait_pending_exit_t *ent =
                        list_entry(pos, linux_wait_pending_exit_t, node);

                list_del_init(&ent->node);
                if (alloc && alloc->m_free)
                        alloc->m_free(alloc, ent);
        }
        INIT_LIST_HEAD(&parent->pending_exits);
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

void linux_proc_wait_wake_for_signal(Thread_Base *thread, linux_proc_resource_t *proc)
{
        Message_Port_t *wait_port;

        if (!thread || !proc)
                return;
        if (thread_get_status(thread) != thread_status_block_on_receive)
                return;
        if (!linux_signal_wait4_should_return_eintr(thread))
                return;

        wait_port = proc_get_or_create_wait_port(proc->pid);
        if (!wait_port)
                return;
        if ((Message_Port_t *)thread->port_ptr != wait_port) {
                ref_put(&wait_port->refcount, free_message_port_ref);
                return;
        }

        (void)linux_proc_wait_post_interrupt(wait_port);
        ref_put(&wait_port->refcount, free_message_port_ref);
}

bool linux_proc_wait_poke(pid_t parent_pid)
{
        Message_Port_t *wait_port;
        bool ok;

        if (parent_pid <= 0)
                return false;

        wait_port = proc_get_or_create_wait_port(parent_pid);
        if (!wait_port)
                return false;

        ok = linux_proc_wait_post_interrupt(wait_port);
        ref_put(&wait_port->refcount, free_message_port_ref);
        return ok;
}

linux_proc_try_result_t linux_proc_try_post_exit_notify(pid_t parent_pid,
                                                        linux_proc_resource_t *child,
                                                        i32 exit_code)
{
        Message_Port_t *wait_port;
        Msg_Data_t *md;
        Message_t *msg;
        error_t err;

        if (parent_pid <= 0 || !child || child->pid <= 0)
                return LINUX_PROC_TRY_FAIL;

        wait_port = proc_get_or_create_wait_port(parent_pid);
        if (!wait_port)
                return LINUX_PROC_TRY_FAIL;

        md = kmsg_create(wait_port->service_id,
                         KMSG_OP_PROC_EXIT_NOTIFY,
                         LINUX_KMSG_FMT_EXIT_NOTIFY,
                         (i64)child->pid,
                         child,
                         exit_code);
        if (!md) {
                ref_put(&wait_port->refcount, free_message_port_ref);
                return LINUX_PROC_TRY_AGAIN;
        }

        msg = create_message_with_msg(md);
        ref_put(&md->refcount, free_msgdata_ref_default);
        if (!msg) {
                ref_put(&wait_port->refcount, free_message_port_ref);
                return LINUX_PROC_TRY_AGAIN;
        }

        err = ipc_system_try_deliver(wait_port, msg, false);
        ref_put(&wait_port->refcount, free_message_port_ref);

        if (err == REND_SUCCESS || err == -E_REND_PORT_CLOSED)
                return LINUX_PROC_TRY_DELIVERED;
        if (err == -E_REND_AGAIN)
                return LINUX_PROC_TRY_AGAIN;
        return LINUX_PROC_TRY_FAIL;
}
