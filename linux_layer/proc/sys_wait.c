#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <modules/log/log.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/ipc_serial.h>

/* External reference to global port table */
extern struct Port_Table* global_port_table;

/* Linux wait4 options */
#define LINUX_WNOHANG    0x00000001 /* Don't block if no child exited */
#define LINUX_WUNTRACED  0x00000002 /* Report stopped children */
#define LINUX_WCONTINUED 0x00000008 /* Report continued children */

/* Linux compat IPC module and opcodes */
#define KMSG_MOD_LINUX_COMPAT  2u
#define KMSG_LINUX_EXIT_NOTIFY 1u

static error_t proc_put_wstatus_helper(Tcb_Base* task, u64 user_wstatus, i32 encoded)
{
        if (!user_wstatus) {
                return REND_SUCCESS;
        }
        if (!task || !task->vs) {
                return -LINUX_EFAULT;
        }
        if (linux_mm_store_to_user(
                    task->vs, user_wstatus, &encoded, sizeof(i32))
            != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        return REND_SUCCESS;
}

/*
 * Wait for a child process to exit using IPC blocking.
 *
 * This implementation uses proc_registry for O(1) PID lookup and IPC for
 * synchronization instead of polling.
 *
 * Supported:
 * - pid > 0: wait for specific child
 * - pid == -1: wait for any child
 * - pid == 0: wait for children in same process group
 * - pid < -1: wait for children in specific process group
 * - WNOHANG: non-blocking mode
 *
 * Not yet supported:
 * - WUNTRACED, WCONTINUED
 * - rusage parameter
 */
i64 sys_wait4(i32 pid, u64 user_wstatus, i32 options, u64 user_rusage)
{
        Tcb_Base* parent = get_cpu_current_task();
        linux_proc_append_t* parent_pa = NULL;

        if (!parent) {
                pr_error("[PROC] wait4: No current task\n");
                return -LINUX_ESRCH;
        }

        parent_pa = linux_proc_append(parent);
        if (!parent_pa) {
                pr_error("[PROC] wait4: No parent proc_append\n");
                return -LINUX_ESRCH;
        }

        pr_debug("[PROC] wait4: PID=%d waiting for pid=%d, options=0x%x\n",
                 parent->pid,
                 pid,
                 options);

        /* Ignore rusage for now */
        if (user_rusage) {
                pr_debug("[PROC] wait4: rusage not supported, ignoring\n");
        }
        (void)user_rusage;

        Tcb_Base* child = NULL;
        linux_proc_append_t* child_pa = NULL;
        pid_t child_pid = INVALID_ID;

        /*
         * Handle different PID options:
         * - pid > 0:   wait for specific child PID
         * - pid == -1: wait for any child
         * - pid == 0:  wait for children in same process group
         * - pid < -1:  wait for children in process group |pid|
         */

        if (pid > 0) {
                /* Wait for specific child PID */
                child = find_task_by_pid(pid);
                if (!child) {
                        pr_debug("[PROC] wait4: Child PID=%d not found\n", pid);
                        return -LINUX_ECHILD;
                }

                child_pa = linux_proc_append(child);
                if (!child_pa) {
                        pr_error("[PROC] wait4: Child has no proc_append\n");
                        return -LINUX_ESRCH;
                }

                /* Check if this is actually our child */
                if (child_pa->ppid != parent->pid) {
                        pr_debug("[PROC] wait4: PID=%d is not our child (ppid=%d)\n",
                                 pid,
                                 child_pa->ppid);
                        return -LINUX_ECHILD;
                }

                child_pid = child->pid;
        } else if (pid == -1) {
                /* Wait for any child */
                child = find_zombie_child(parent->pid);
                if (!child) {
                        pr_debug(
                                "[PROC] wait4: No zombie children found for parent PID=%d\n",
                                parent->pid);
                        return -LINUX_ECHILD;
                }

                child_pa = linux_proc_append(child);
                if (!child_pa) {
                        pr_error("[PROC] wait4: Child has no proc_append\n");
                        return -LINUX_ESRCH;
                }

                child_pid = child->pid;
        } else if (pid == 0) {
                /* Wait for children in same process group */
                if (!parent_pa) {
                        pr_error("[PROC] wait4: Parent has no proc_append\n");
                        return -LINUX_ESRCH;
                }

                child = find_zombie_child_in_pgid(parent->pid, parent_pa->pgid);
                if (!child) {
                        pr_debug(
                                "[PROC] wait4: No zombie children found in pgid %d\n",
                                parent_pa->pgid);
                        return -LINUX_ECHILD;
                }

                child_pa = linux_proc_append(child);
                if (!child_pa) {
                        pr_error("[PROC] wait4: Child has no proc_append\n");
                        return -LINUX_ESRCH;
                }

                child_pid = child->pid;
        } else { /* pid < -1 */
                /* Wait for children in specific process group */
                pid_t pgid = -pid;
                child = find_zombie_child_in_pgid(parent->pid, pgid);
                if (!child) {
                        pr_debug(
                                "[PROC] wait4: No zombie children found in pgid %d\n",
                                pgid);
                        return -LINUX_ECHILD;
                }

                child_pa = linux_proc_append(child);
                if (!child_pa) {
                        pr_error("[PROC] wait4: Child has no proc_append\n");
                        return -LINUX_ESRCH;
                }

                child_pid = child->pid;
        }

        /* Check if child already exited (zombie) */
        if (child_pa->exit_state == 1) {
                i32 exit_code = child_pa->exit_code;

                pr_debug("[PROC] wait4: Child PID=%d already exited (zombie)\n",
                         child_pid);

                /* Mark child as reaped */
                child_pa->exit_state = 2; /* 2 = reaped */

                /*
                 * Encode exit status in Linux format.
                 * For normal exit: status = (exit_code << 8) | 0x00
                 * This matches WEXITSTATUS(status) = ((status & 0xff00) >> 8)
                 */
                i32 encoded_status = 0;
                if (exit_code >= 0 && exit_code <= 255) {
                        encoded_status = (exit_code << 8) | 0x00;
                } else {
                        encoded_status = (255 << 8) | 0x00;
                }

                if (proc_put_wstatus_helper(parent, user_wstatus, encoded_status)
                    != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }

                /*
                 * Note: We don't need to notify clean_server here.
                 * The child process already notified clean_server when it exited.
                 * clean_server will check exit_state and decide whether to delete:
                 * - exit_state==1 (zombie) with parent: keep for wait4
                 * - exit_state==1 (zombie) orphan: delete immediately
                 * - exit_state==2 (reaped): delete immediately
                 */
                pr_debug("[PROC] wait4: Child PID=%d marked as reaped, exit_code=%d, status=0x%x\n",
                         child_pid, exit_code, encoded_status);

                return (i64)child_pid;
        }

        /* Handle WNOHANG: non-blocking check */
        if (options & LINUX_WNOHANG) {
                pr_debug("[PROC] wait4: WNOHANG: child still running\n");
                return 0;
        }

        /* For pid > 0, use IPC blocking wait */
        if (pid > 0) {
                /* Create wait port and block for exit message */
                char port_name[PROC_WAIT_PORT_NAME_MAX];
                Message_Port_t* wait_port;

                proc_format_wait_port_name(port_name, sizeof(port_name),
                                           parent->pid);
                wait_port = proc_get_or_create_wait_port(parent->pid);
                if (!wait_port) {
                        pr_error("[PROC] wait4: Failed to get wait port\n");
                        return -LINUX_EAGAIN;
                }

                pr_debug(
                        "[PROC] wait4: PID=%d waiting on port '%s' for child PID=%d\n",
                        parent->pid,
                        port_name,
                        child_pid);

                /*
                 * thread_lookup_port / port_table_lookup success holds one ref
                 * (see doc/ai/INVARIANTS.md); create path returns create ref.
                 */
                error_t recv_e = recv_msg(wait_port);
                if (recv_e != REND_SUCCESS) {
                        pr_error("[PROC] wait4: Failed to receive message (e=%d)\n",
                                 (int)recv_e);
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return -LINUX_EINTR;
                }

                Message_t* msg = dequeue_recv_msg();
                if (!msg) {
                        pr_error("[PROC] wait4: Failed to dequeue message\n");
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return -LINUX_EINTR;
                }

                /* Extract exit information from message data */
                const kmsg_t* kmsg = kmsg_from_msg(msg);
                if (!kmsg || kmsg->hdr.magic != KMSG_MAGIC) {
                        pr_error("[PROC] wait4: Invalid kmsg magic\n");
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return -LINUX_ECHILD;
                }

                /* Verify module and opcode */
                if (kmsg->hdr.module != KMSG_MOD_LINUX_COMPAT
                    || kmsg->hdr.opcode != KMSG_LINUX_EXIT_NOTIFY) {
                        pr_error(
                                "[PROC] wait4: Invalid kmsg module/opcode (mod=%u, op=%u)\n",
                                kmsg->hdr.module,
                                kmsg->hdr.opcode);
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return -LINUX_ECHILD;
                }

                /* Decode the payload: format "qi" = i64(pid) + i32(exit_code)
                 */
                i64 child_pid_i64;
                i32 exit_code;
                error_t dec_e = ipc_serial_decode(kmsg->payload,
                                                  kmsg->hdr.payload_len,
                                                  "qi",
                                                  &child_pid_i64,
                                                  &exit_code);
                if (dec_e != REND_SUCCESS) {
                        pr_error("[PROC] wait4: Failed to decode payload (e=%d)\n",
                                 (int)dec_e);
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return -LINUX_ECHILD;
                }

                ref_put(&msg->ms_queue_node.refcount, free_message_ref);

                pr_debug(
                        "[PROC] wait4: Received exit notification: child_pid=%d, exit_code=%d\n",
                        (pid_t)child_pid_i64,
                        exit_code);

                /*
                 * Encode exit status in Linux format.
                 * For normal exit: status = (exit_code << 8) | 0x00
                 */
                i32 encoded_status = 0;
                if (exit_code >= 0 && exit_code <= 255) {
                        encoded_status = (exit_code << 8) | 0x00;
                } else {
                        /* Exit code out of range, use 0xFF */
                        encoded_status = (255 << 8) | 0x00;
                }

                if (proc_put_wstatus_helper(parent, user_wstatus, encoded_status)
                    != REND_SUCCESS) {
                        ref_put(&wait_port->refcount, free_message_port_ref);
                        return -LINUX_EFAULT;
                }

                ref_put(&wait_port->refcount, free_message_port_ref);

                pr_debug("[PROC] wait4: Returning child_pid=%d, exit_code=%d, status=0x%x\n",
                         (pid_t)child_pid_i64, exit_code, encoded_status);

                return (i64)child_pid_i64;
        } else {
                /* For pid == -1, 0, or < -1, we should have found a zombie
                 * above */
                pr_error("[PROC] wait4: Unexpected: non-zombie child for pid=%d\n",
                         pid);
                return -LINUX_ECHILD;
        }
}
