#include <linux_compat/errno.h>
#include <linux_compat/ipc/block_wake.h>
#include <linux_compat/ipc/exit_protocol.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc/clean_ipc.h>
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
#include <rendezvos/sync/cas_lock.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

#define LINUX_WNOHANG    0x00000001
#define LINUX_WUNTRACED  0x00000002
#define LINUX_WCONTINUED 0x00000008

/*
 * wait4 model (notify-only):
 *   child: exit_state=1, then THREAD_REAP
 *   clean worker: delete_thread; if last && zombie → blocking EXIT_NOTIFY
 *   parent: pending / recv EXIT_NOTIFY → exit_state=2 → TASK_REAP
 * clean_server listen loop uses per-message workers so EXIT_NOTIFY can block.
 */

static error_t proc_put_wstatus_helper(Tcb_Base *task, u64 user_wstatus,
                                       i32 encoded)
{
        if (!user_wstatus)
                return REND_SUCCESS;
        if (!task || !task->vs)
                return -LINUX_EFAULT;
        if (linux_mm_store_to_user(task->vs, user_wstatus, &encoded, sizeof(i32))
            != REND_SUCCESS)
                return -LINUX_EFAULT;
        return REND_SUCCESS;
}

static i32 wait4_encode_status(i32 exit_code)
{
        if (exit_code >= 0 && exit_code <= 255)
                return (exit_code << 8) | 0x00;
        return (255 << 8) | 0x00;
}

static bool wait4_pid_matches(i32 want_pid, pid_t child_pid, Tcb_Base *parent,
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

/*
 * After EXIT_NOTIFY: mark reaped and ask clean_server to delete_task.
 * Lookup is by the pid carried in the notify, not a zombie registry scan.
 */
static i64 wait4_finish_reap(Tcb_Base *parent, u64 user_wstatus,
                             pid_t child_pid, i32 exit_code)
{
        Tcb_Base *child;
        linux_proc_append_t *child_pa;
        i32 encoded_status;
        bool task_empty;

        child = find_task_by_pid(child_pid);
        if (!child)
                return -LINUX_ECHILD;
        child_pa = linux_proc_append(child);
        if (!child_pa || child_pa->ppid != parent->pid
            || child_pa->exit_state != 1)
                return -LINUX_ECHILD;

        lock_cas(&child->thread_list_lock);
        task_empty = (child->thread_number == 0);
        if (!task_empty) {
                unlock_cas(&child->thread_list_lock);
                return 0;
        }
        child_pa->exit_code = exit_code;
        child_pa->exit_state = 2;
        unlock_cas(&child->thread_list_lock);

        encoded_status = wait4_encode_status(exit_code);
        if (proc_put_wstatus_helper(parent, user_wstatus, encoded_status)
            != REND_SUCCESS) {
                child_pa->exit_state = 1;
                return -LINUX_EFAULT;
        }

        (void)linux_clean_send_task_reap(child_pid);
        return (i64)child_pid;
}

static bool wait4_has_live_child(i32 pid, Tcb_Base *parent,
                                 linux_proc_append_t *parent_pa)
{
        if (pid > 0) {
                Tcb_Base *child = find_task_by_pid(pid);
                linux_proc_append_t *child_pa;

                if (!child)
                        return false;
                child_pa = linux_proc_append(child);
                return child_pa && child_pa->ppid == parent->pid
                       && child_pa->exit_state != 2;
        }
        if (pid == -1)
                return proc_parent_has_unreaped_child(parent->pid, 0, false);
        if (pid == 0) {
                if (!parent_pa)
                        return false;
                return proc_parent_has_unreaped_child(
                        parent->pid, parent_pa->pgid, true);
        }
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
                                     pid_t *child_pid_out, i32 *exit_code_out)
{
        const kmsg_t *km;
        i64 child_pid_i64;
        i32 exit_code;

        if (!wait_port || !msg || !child_pid_out || !exit_code_out)
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
                              &exit_code)
            != REND_SUCCESS)
                return false;

        *child_pid_out = (pid_t)child_pid_i64;
        *exit_code_out = exit_code;
        return true;
}

/*
 * Handle one dequeued wait_port message.
 * Returns >0 reaped pid, <0 errno, or 0 to keep waiting.
 */
static i64 wait4_handle_port_msg(Tcb_Base *parent,
                                 linux_proc_append_t *parent_pa, i32 want_pid,
                                 u64 user_wstatus, Message_Port_t *wait_port,
                                 Message_t *msg, Thread_Base *self)
{
        pid_t child_pid;
        i32 exit_code;

        if (wait4_recv_is_interrupt(wait_port, msg)) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                if (self && linux_signal_wait4_should_return_eintr(self))
                        return -LINUX_EINTR;
                return 0;
        }

        if (linux_ipc_kmsg_is_port_closed(wait_port, msg)) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                return -LINUX_EINTR;
        }

        if (!wait4_decode_exit_notify(wait_port, msg, &child_pid, &exit_code)) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                return 0;
        }
        ref_put(&msg->ms_queue_node.refcount, free_message_ref);

        if (!wait4_pid_matches(want_pid, child_pid, parent, parent_pa)) {
                if (!linux_proc_wait_pending_push(parent_pa, child_pid,
                                                 exit_code)) {
                        pr_error(
                                "[PROC] wait4: pending EXIT_NOTIFY drop pid=%d\n",
                                (int)child_pid);
                }
                return 0;
        }

        return wait4_finish_reap(parent, user_wstatus, child_pid, exit_code);
}

static i64 wait4_try_pending(Tcb_Base *parent, linux_proc_append_t *parent_pa,
                             i32 want_pid, u64 user_wstatus)
{
        pid_t child_pid;
        i32 exit_code;

        if (!linux_proc_wait_pending_take(parent_pa, want_pid, parent,
                                         &child_pid, &exit_code))
                return 0;
        return wait4_finish_reap(parent, user_wstatus, child_pid, exit_code);
}

static i64 wait4_try_recv_once(Tcb_Base *parent, linux_proc_append_t *parent_pa,
                               i32 want_pid, u64 user_wstatus,
                               Message_Port_t *wait_port, Thread_Base *self)
{
        Message_t *msg;
        error_t recv_e;
        i64 ret;

        recv_e = ipc_try_recv_msg(wait_port);
        if (recv_e != REND_SUCCESS)
                return 0;

        msg = dequeue_recv_msg();
        if (!msg)
                return 0;

        ret = wait4_handle_port_msg(parent, parent_pa, want_pid, user_wstatus,
                                    wait_port, msg, self);
        return ret;
}

static i64 wait4_block_on_port(Tcb_Base *parent, linux_proc_append_t *parent_pa,
                               i32 pid, u64 user_wstatus)
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

                ret = wait4_try_pending(parent, parent_pa, pid, user_wstatus);
                if (ret != 0) {
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return ret;
                }

                if (self && linux_signal_wait4_should_return_eintr(self)) {
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return -LINUX_EINTR;
                }

                /* Prefer try_recv so a just-queued IPC notify is taken before
                 * blocking; then recheck pending (try_deliver miss race). */
                recv_e = ipc_try_recv_msg(wait_port);
                if (recv_e == REND_SUCCESS) {
                        msg = dequeue_recv_msg();
                        if (!msg)
                                continue;
                        ret = wait4_handle_port_msg(parent, parent_pa, pid,
                                                    user_wstatus, wait_port,
                                                    msg, self);
                        if (ret != 0) {
                                ref_put(&wait_port->refcount,
                                        free_message_port_ref);
                                return ret;
                        }
                        continue;
                }

                ret = wait4_try_pending(parent, parent_pa, pid, user_wstatus);
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

                ret = wait4_handle_port_msg(parent, parent_pa, pid,
                                            user_wstatus, wait_port, msg, self);
                if (ret != 0) {
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return ret;
                }
        }
}

/*
 * Supported: pid > 0 / -1 / 0 / < -1, WNOHANG.
 * Not yet: WUNTRACED, WCONTINUED, rusage, waitid siginfo_t fill-in.
 */
i64 sys_wait4(i32 pid, u64 user_wstatus, i32 options, u64 user_rusage)
{
        Tcb_Base *parent = get_cpu_current_task();
        linux_proc_append_t *parent_pa;
        Message_Port_t *wait_port;
        Thread_Base *self;
        i64 ret;

        (void)user_rusage;

        if (!parent) {
                pr_error("[PROC] wait4: No current task\n");
                return -LINUX_ESRCH;
        }

        parent_pa = linux_proc_append(parent);
        if (!parent_pa) {
                pr_error("[PROC] wait4: No parent proc_append\n");
                return -LINUX_ESRCH;
        }

        ret = wait4_try_pending(parent, parent_pa, pid, user_wstatus);
        if (ret != 0)
                return ret;

        wait_port = proc_get_or_create_wait_port(parent->pid);
        if (!wait_port) {
                pr_error("[PROC] wait4: Failed to get wait port\n");
                return -LINUX_EAGAIN;
        }

        self = get_cpu_current_thread();
        ret = wait4_try_recv_once(parent, parent_pa, pid, user_wstatus,
                                  wait_port, self);
        if (ret != 0) {
                ref_put(&wait_port->refcount, free_message_port_ref);
                return ret;
        }

        if (options & LINUX_WNOHANG) {
                if (!wait4_has_live_child(pid, parent, parent_pa)) {
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return -LINUX_ECHILD;
                }
                ref_put(&wait_port->refcount, free_message_port_ref);
                return 0;
        }

        if (!wait4_has_live_child(pid, parent, parent_pa)) {
                ref_put(&wait_port->refcount, free_message_port_ref);
                return -LINUX_ECHILD;
        }

        ref_put(&wait_port->refcount, free_message_port_ref);
        return wait4_block_on_port(parent, parent_pa, pid, user_wstatus);
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
