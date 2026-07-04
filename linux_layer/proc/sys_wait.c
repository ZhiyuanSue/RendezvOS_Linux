#include <linux_compat/errno.h>
#include <linux_compat/ipc/exit_protocol.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/signal/signal_deliver.h>
#include <modules/log/log.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

extern struct Port_Table* global_port_table;

#define LINUX_WNOHANG    0x00000001
#define LINUX_WUNTRACED  0x00000002
#define LINUX_WCONTINUED 0x00000008

/*
 * wait4 / waitid (linux_layer)
 *
 * Source of truth for reap: linux_proc_append_t.exit_state / exit_code
 * (set in sys_exit before the parent is woken).
 *
 * IPC (per-process wait_port) blocks until child sys_exit posts EXIT_NOTIFY or
 * a deliverable signal posts WAIT_INTERRUPT (EINTR). Reap always via
 * exit_state.
 */

static error_t proc_put_wstatus_helper(Tcb_Base* task, u64 user_wstatus,
                                       i32 encoded)
{
        if (!user_wstatus) {
                return REND_SUCCESS;
        }
        if (!task || !task->vs) {
                return -LINUX_EFAULT;
        }
        if (linux_mm_store_to_user(task->vs, user_wstatus, &encoded, sizeof(i32))
            != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        return REND_SUCCESS;
}

static i32 wait4_encode_status(i32 exit_code)
{
        if (exit_code >= 0 && exit_code <= 255) {
                return (exit_code << 8) | 0x00;
        }
        return (255 << 8) | 0x00;
}

static i64 wait4_reap_child(Tcb_Base* parent, u64 user_wstatus, Tcb_Base* child,
                            linux_proc_append_t* child_pa)
{
        i32 encoded_status = wait4_encode_status(child_pa->exit_code);

        child_pa->exit_state = 2;

        if (proc_put_wstatus_helper(parent, user_wstatus, encoded_status)
            != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        return (i64)child->pid;
}

static Tcb_Base* wait4_find_zombie(i32 pid, Tcb_Base* parent,
                                   linux_proc_append_t* parent_pa)
{
        if (pid > 0) {
                Tcb_Base* child = find_task_by_pid(pid);
                linux_proc_append_t* child_pa;

                if (!child) {
                        return NULL;
                }
                child_pa = linux_proc_append(child);
                if (!child_pa || child_pa->ppid != parent->pid
                    || child_pa->exit_state != 1) {
                        return NULL;
                }
                return child;
        }
        if (pid == -1) {
                return find_zombie_child(parent->pid);
        }
        if (pid == 0) {
                if (!parent_pa) {
                        return NULL;
                }
                return find_zombie_child_in_pgid(parent->pid, parent_pa->pgid);
        }
        return find_zombie_child_in_pgid(parent->pid, -pid);
}

static bool wait4_has_live_child(i32 pid, Tcb_Base* parent,
                                 linux_proc_append_t* parent_pa)
{
        if (pid > 0) {
                Tcb_Base* child = find_task_by_pid(pid);
                linux_proc_append_t* child_pa;

                if (!child) {
                        return false;
                }
                child_pa = linux_proc_append(child);
                return child_pa && child_pa->ppid == parent->pid
                       && child_pa->exit_state != 2;
        }
        if (pid == -1) {
                return proc_parent_has_unreaped_child(parent->pid, 0, false);
        }
        if (pid == 0) {
                if (!parent_pa) {
                        return false;
                }
                return proc_parent_has_unreaped_child(
                        parent->pid, parent_pa->pgid, true);
        }
        return proc_parent_has_unreaped_child(parent->pid, -pid, true);
}

static void wait4_drain_recv_queue(void)
{
        Message_t* msg;

        while ((msg = dequeue_recv_msg()) != NULL) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
        }
}

static i64 wait4_reap_zombie_or(Tcb_Base* parent,
                                linux_proc_append_t* parent_pa, i32 pid,
                                u64 user_wstatus, i64 on_none)
{
        Tcb_Base* zombie = wait4_find_zombie(pid, parent, parent_pa);
        linux_proc_append_t* zombie_pa;

        if (!zombie) {
                return on_none;
        }
        zombie_pa = linux_proc_append(zombie);
        if (!zombie_pa) {
                return -LINUX_ESRCH;
        }
        return wait4_reap_child(parent, user_wstatus, zombie, zombie_pa);
}

static bool wait4_recv_is_interrupt(Message_Port_t* wait_port, Message_t* msg)
{
        const kmsg_t* km;

        if (!wait_port || !msg) {
                return false;
        }

        km = kmsg_from_msg(msg);
        if (!km || km->hdr.module != wait_port->service_id) {
                return false;
        }
        return km->hdr.opcode == KMSG_OP_PROC_WAIT_INTERRUPT;
}

static i64 wait4_block_on_port(Tcb_Base* parent, linux_proc_append_t* parent_pa,
                               i32 pid, u64 user_wstatus)
{
        Message_Port_t* wait_port;
        Thread_Base* self = get_cpu_current_thread();
        i64 reap_ret;

        wait_port = proc_get_or_create_wait_port(parent->pid);
        if (!wait_port) {
                pr_error("[PROC] wait4: Failed to get wait port\n");
                return -LINUX_EAGAIN;
        }

        for (;;) {
                Message_t* msg;

                reap_ret = wait4_reap_zombie_or(
                        parent, parent_pa, pid, user_wstatus, 0);
                if (reap_ret != 0) {
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return reap_ret;
                }

                if (self && linux_signal_thread_has_deliverable_pending(self)) {
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return -LINUX_EINTR;
                }

                error_t recv_e = recv_msg(wait_port);
                if (recv_e != REND_SUCCESS) {
                        reap_ret = wait4_reap_zombie_or(
                                parent, parent_pa, pid, user_wstatus, 0);
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        if (reap_ret != 0) {
                                return reap_ret;
                        }
                        pr_error(
                                "[PROC] wait4: recv_msg on wait_port failed e=%d\n",
                                (int)recv_e);
                        return -LINUX_EINTR;
                }

                msg = dequeue_recv_msg();
                if (!msg) {
                        continue;
                }

                if (wait4_recv_is_interrupt(wait_port, msg)) {
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return -LINUX_EINTR;
                }

                if (self && linux_signal_thread_has_deliverable_pending(self)) {
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return -LINUX_EINTR;
                }

                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                wait4_drain_recv_queue();
        }
}

/*
 * Supported: pid > 0 / -1 / 0 / < -1, WNOHANG.
 * Not yet: WUNTRACED, WCONTINUED, rusage, waitid siginfo_t fill-in.
 */
i64 sys_wait4(i32 pid, u64 user_wstatus, i32 options, u64 user_rusage)
{
        Tcb_Base* parent = get_cpu_current_task();
        linux_proc_append_t* parent_pa;
        i64 reap_ret;

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

        reap_ret =
                wait4_reap_zombie_or(parent, parent_pa, pid, user_wstatus, 0);
        if (reap_ret != 0) {
                return reap_ret;
        }

        if (options & LINUX_WNOHANG) {
                return 0;
        }

        if (!wait4_has_live_child(pid, parent, parent_pa)) {
                return -LINUX_ECHILD;
        }

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
