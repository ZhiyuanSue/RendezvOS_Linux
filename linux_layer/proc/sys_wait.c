#include <linux_compat/errno.h>
#include <linux_compat/ipc/block_wake.h>
#include <linux_compat/ipc/exit_protocol.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc/wait_ipc.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/signal/signal_deliver.h>
#include <modules/log/log.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/ipc_serial.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/thread.h>
#include <syscall.h>

#define LINUX_WNOHANG    0x00000001
#define LINUX_WUNTRACED  0x00000002
#define LINUX_WCONTINUED 0x00000008

/*
 * wait4 — protocols/EXIT_CLEAN.md
 *   clean: EXIT_NOTIFY carries proc* handoff
 *   parent: decode proc* → wstatus → linux_proc_reap (ref_put, no RPC)
 */

static error_t wait4_put_wstatus(linux_proc_resource_t *parent, u64 user_wstatus,
                                 i32 encoded)
{
        if (!user_wstatus)
                return REND_SUCCESS;
        if (!parent || !linux_current_vs())
                return -LINUX_EFAULT;
        if (linux_mm_store_to_user(linux_current_vs(), user_wstatus, &encoded, sizeof(i32))
            != REND_SUCCESS)
                return -LINUX_EFAULT;
        return REND_SUCCESS;
}

static i32 wait4_encode_status(i32 exit_code)
{
        return ((exit_code & 0xff) << 8) | 0x00;
}

static i64 wait4_finish_reap(linux_proc_resource_t *parent, u64 user_wstatus,
                             linux_proc_resource_t *child, i32 exit_code)
{
        i32 encoded_status;
        error_t reap_e;
        pid_t child_pid;

        if (!child || child->pid <= 0)
                return -LINUX_ECHILD;

        child_pid = child->pid;
        if (child->ppid != parent->pid
            || (child->exit_state != LINUX_EXIT_ZOMBIE
                && child->exit_state != LINUX_EXIT_NOTIFIED)
            || !child->exit_last_thread)
                return -LINUX_ECHILD;

        child->exit_code = exit_code;
        encoded_status = wait4_encode_status(exit_code);
        if (wait4_put_wstatus(parent, user_wstatus, encoded_status) != REND_SUCCESS)
                return -LINUX_EFAULT;

        reap_e = linux_proc_reap(child);
        if (reap_e != REND_SUCCESS && find_proc_by_pid(child_pid) != NULL)
                return -LINUX_EAGAIN;

        return (i64)child_pid;
}

static bool wait4_has_live_child(i32 pid, linux_proc_resource_t *parent)
{
        if (pid > 0) {
                linux_proc_resource_t *child = find_proc_by_pid(pid);

                return child && child->ppid == parent->pid
                       && (child->exit_state == LINUX_EXIT_ZOMBIE
                           || child->exit_state == LINUX_EXIT_NOTIFIED
                           || child->exit_state == LINUX_EXIT_RUNNING);
        }
        if (pid == -1)
                return proc_parent_has_unreaped_child(parent->pid, 0, false);
        if (pid == 0)
                return proc_parent_has_unreaped_child(parent->pid, parent->pgid, true);
        return proc_parent_has_unreaped_child(parent->pid, -pid, true);
}

static bool wait4_recv_is_interrupt(Message_Port_t *wait_port, Message_t *msg)
{
        const kmsg_t *km;

        if (!wait_port || !msg)
                return false;

        km = kmsg_from_msg(msg);
        if (!km || km->hdr.module != wait_port->service_id)
                return false;
        return km->hdr.opcode == KMSG_OP_PROC_WAIT_INTERRUPT;
}

static bool wait4_decode_exit_notify(Message_Port_t *wait_port, Message_t *msg,
                                     linux_proc_resource_t **child_out,
                                     pid_t *child_pid_out, i32 *exit_code_out)
{
        const kmsg_t *km;
        i64 child_pid_i64;
        void *vchild;
        i32 exit_code;
        linux_proc_resource_t *child;

        if (!wait_port || !msg || !child_out || !child_pid_out || !exit_code_out)
                return false;

        km = kmsg_from_msg(msg);
        if (!km || km->hdr.module != wait_port->service_id)
                return false;
        if (km->hdr.opcode != KMSG_OP_PROC_EXIT_NOTIFY)
                return false;
        if (ipc_serial_decode(km->payload,
                              km->hdr.payload_len,
                              LINUX_KMSG_FMT_EXIT_NOTIFY,
                              &child_pid_i64,
                              &vchild,
                              &exit_code)
            != REND_SUCCESS)
                return false;

        child = (linux_proc_resource_t *)vchild;
        if (!child || child->pid != (pid_t)child_pid_i64 || child_pid_i64 <= 0)
                return false;

        *child_out = child;
        *child_pid_out = (pid_t)child_pid_i64;
        *exit_code_out = exit_code;
        return true;
}

static i64 wait4_try_pending(linux_proc_resource_t *parent, i32 want_pid,
                             u64 user_wstatus)
{
        linux_proc_resource_t *child;
        i32 exit_code;

        if (!linux_proc_wait_pending_take(parent, want_pid, &child, &exit_code))
                return 0;
        return wait4_finish_reap(parent, user_wstatus, child, exit_code);
}

static i64 wait4_handle_port_msg(linux_proc_resource_t *parent, i32 want_pid,
                                 u64 user_wstatus, Message_Port_t *wait_port,
                                 Message_t *msg, Thread_Base *self)
{
        linux_proc_resource_t *child;
        pid_t child_pid;
        i32 exit_code;

        if (wait4_recv_is_interrupt(wait_port, msg)) {
                i64 pending_ret;

                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                pending_ret = wait4_try_pending(parent, want_pid, user_wstatus);
                if (pending_ret != 0)
                        return pending_ret;
                if (self && linux_signal_wait4_should_return_eintr(self))
                        return -LINUX_EINTR;
                return 0;
        }

        if (linux_ipc_kmsg_is_port_closed(wait_port, msg)) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                return -LINUX_EINTR;
        }

        if (!wait4_decode_exit_notify(
                    wait_port, msg, &child, &child_pid, &exit_code)) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                return 0;
        }
        ref_put(&msg->ms_queue_node.refcount, free_message_ref);

        if (!linux_proc_wait_pid_matches(want_pid, child_pid, parent)) {
                if (!linux_proc_wait_pending_push(parent, child, exit_code)) {
                        pr_error(
                                "[PROC] wait4: pending EXIT_NOTIFY drop pid=%d\n",
                                (int)child_pid);
                }
                return 0;
        }

        return wait4_finish_reap(parent, user_wstatus, child, exit_code);
}

static i64 wait4_block_on_port(linux_proc_resource_t *parent, i32 pid,
                               u64 user_wstatus)
{
        Message_Port_t *wait_port;
        Thread_Base *self = get_cpu_current_thread();
        i64 ret;

        wait_port = proc_get_or_create_wait_port(parent->pid);
        if (!wait_port) {
                pr_error("[PROC] wait4: Failed to get wait port\n");
                return -LINUX_EAGAIN;
        }

        for (;;) {
                Message_t *msg;
                error_t recv_e;

                ret = wait4_try_pending(parent, pid, user_wstatus);
                if (ret != 0) {
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return ret;
                }

                if (self && linux_signal_wait4_should_return_eintr(self)) {
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return -LINUX_EINTR;
                }

                recv_e = ipc_try_recv_msg(wait_port);
                if (recv_e == REND_SUCCESS) {
                        msg = dequeue_recv_msg();
                        if (!msg)
                                continue;
                        ret = wait4_handle_port_msg(
                                parent, pid, user_wstatus, wait_port, msg, self);
                        if (ret != 0) {
                                ref_put(&wait_port->refcount,
                                        free_message_port_ref);
                                return ret;
                        }
                        continue;
                }

                ret = wait4_try_pending(parent, pid, user_wstatus);
                if (ret != 0) {
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return ret;
                }

                recv_e = recv_msg(wait_port);
                if (recv_e == -E_REND_PORT_CLOSED) {
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return -LINUX_EINTR;
                }
                if (recv_e != REND_SUCCESS) {
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        pr_error(
                                "[PROC] wait4: recv_msg on wait_port failed e=%d\n",
                                (int)recv_e);
                        return -LINUX_EINTR;
                }

                msg = dequeue_recv_msg();
                if (!msg)
                        continue;

                ret = wait4_handle_port_msg(
                        parent, pid, user_wstatus, wait_port, msg, self);
                if (ret != 0) {
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return ret;
                }
        }
}

i64 sys_wait4(i32 pid, u64 user_wstatus, i32 options, u64 user_rusage)
{
        linux_proc_resource_t *parent = linux_current_proc();
        Message_Port_t *wait_port;
        Thread_Base *self;
        i64 ret;

        (void)user_rusage;

        if (!parent) {
                pr_error("[PROC] wait4: No current task\n");
                return -LINUX_ESRCH;
        }

        ret = wait4_try_pending(parent, pid, user_wstatus);
        if (ret != 0)
                return ret;

        wait_port = proc_get_or_create_wait_port(parent->pid);
        if (!wait_port) {
                pr_error("[PROC] wait4: Failed to get wait port\n");
                return -LINUX_EAGAIN;
        }

        self = get_cpu_current_thread();
        if (ipc_try_recv_msg(wait_port) == REND_SUCCESS) {
                Message_t *msg = dequeue_recv_msg();

                if (msg) {
                        ret = wait4_handle_port_msg(
                                parent, pid, user_wstatus, wait_port, msg, self);
                        if (ret != 0) {
                                ref_put(&wait_port->refcount, free_message_port_ref);
                                return ret;
                        }
                }
        }

        if (options & LINUX_WNOHANG) {
                ret = wait4_has_live_child(pid, parent) ? 0 : -LINUX_ECHILD;
                ref_put(&wait_port->refcount, free_message_port_ref);
                return ret;
        }

        if (!wait4_has_live_child(pid, parent)) {
                ref_put(&wait_port->refcount, free_message_port_ref);
                return -LINUX_ECHILD;
        }

        ref_put(&wait_port->refcount, free_message_port_ref);
        return wait4_block_on_port(parent, pid, user_wstatus);
}

#define LINUX_WAITID_P_ALL  0
#define LINUX_WAITID_P_PID  1
#define LINUX_WAITID_P_PGID 2

i64 sys_waitid(i32 idtype, u32 id, u64 infop, i32 options)
{
        i32 pid;

        (void)infop;

        switch (idtype) {
        case LINUX_WAITID_P_PID:
                pid = (i32)id;
                break;
        case LINUX_WAITID_P_ALL:
                pid = -1;
                break;
        case LINUX_WAITID_P_PGID:
                pid = -(i32)id;
                break;
        default:
                return -LINUX_ENOSYS;
        }

        return sys_wait4(pid, 0, options, 0);
}
