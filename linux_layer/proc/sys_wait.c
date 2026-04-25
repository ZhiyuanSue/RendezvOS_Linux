#include <common/types.h>
#include <linux_compat/errno.h>
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

/* Wait port name format: "wait_port_<pid>" */
#define WAIT_PORT_NAME_MAX 32

/* Linux compat IPC module and opcodes */
#define KMSG_MOD_LINUX_COMPAT      2u
#define KMSG_LINUX_EXIT_NOTIFY     1u

/*
 * Generate wait port name for a process.
 * buf must have at least WAIT_PORT_NAME_MAX bytes.
 */
static void make_wait_port_name(char* buf, size_t bufsize, pid_t pid)
{
        /* Format: "wait_port_<pid>" */
        const char* prefix = "wait_port_";
        size_t prefix_len = 10; /* strlen("wait_port_") */

        if (bufsize < prefix_len + 1) {
                buf[0] = '\0';
                return;
        }

        /* Copy prefix */
        for (size_t i = 0; i < prefix_len; i++) {
                buf[i] = prefix[i];
        }

        /* Append PID (simple decimal) */
        pid_t temp_pid = pid;
        size_t idx = prefix_len;

        if (temp_pid == 0) {
                if (idx < bufsize - 1) {
                        buf[idx++] = '0';
                }
        } else {
                char rev[16];
                size_t rev_len = 0;
                while (temp_pid > 0 && rev_len < sizeof(rev)) {
                        rev[rev_len++] = '0' + (temp_pid % 10);
                        temp_pid /= 10;
                }
                for (size_t i = 0; i < rev_len && idx < bufsize - 1; i++) {
                        buf[idx++] = rev[rev_len - 1 - i];
                }
        }

        buf[idx] = '\0';
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
i64 sys_wait4(i32 pid, i64* wstatus, i32 options, i64* rusage)
{
        Tcb_Base* parent = get_cpu_current_task();
        linux_proc_append_t* parent_pa = NULL;

        if (!parent) {
                pr_error("[wait4] No current task\n");
                return -LINUX_ESRCH;
        }

        parent_pa = linux_proc_append(parent);
        if (!parent_pa) {
                pr_error("[wait4] No parent proc_append\n");
                return -LINUX_ESRCH;
        }

        pr_debug("[wait4] PID=%d waiting for pid=%d, options=0x%x\n",
                 parent->pid,
                 pid,
                 options);

        /* Ignore rusage for now */
        if (rusage) {
                pr_debug("[wait4] rusage not supported, ignoring\n");
        }

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
                        pr_debug("[wait4] Child PID=%d not found\n", pid);
                        return -LINUX_ECHILD;
                }

                child_pa = linux_proc_append(child);
                if (!child_pa) {
                        pr_error("[wait4] Child has no proc_append\n");
                        return -LINUX_ESRCH;
                }

                /* Check if this is actually our child */
                if (child_pa->ppid != parent->pid) {
                        pr_debug("[wait4] PID=%d is not our child (ppid=%d)\n",
                                 pid, child_pa->ppid);
                        return -LINUX_ECHILD;
                }

                child_pid = child->pid;
        } else if (pid == -1) {
                /* Wait for any child */
                child = find_zombie_child(parent->pid);
                if (!child) {
                        pr_debug("[wait4] No zombie children found for parent PID=%d\n",
                                 parent->pid);
                        return -LINUX_ECHILD;
                }

                child_pa = linux_proc_append(child);
                if (!child_pa) {
                        pr_error("[wait4] Child has no proc_append\n");
                        return -LINUX_ESRCH;
                }

                child_pid = child->pid;
        } else if (pid == 0) {
                /* Wait for children in same process group */
                if (!parent_pa) {
                        pr_error("[wait4] Parent has no proc_append\n");
                        return -LINUX_ESRCH;
                }

                child = find_zombie_child_in_pgid(parent->pid, parent_pa->pgid);
                if (!child) {
                        pr_debug("[wait4] No zombie children found in pgid %d\n",
                                 parent_pa->pgid);
                        return -LINUX_ECHILD;
                }

                child_pa = linux_proc_append(child);
                if (!child_pa) {
                        pr_error("[wait4] Child has no proc_append\n");
                        return -LINUX_ESRCH;
                }

                child_pid = child->pid;
        } else { /* pid < -1 */
                /* Wait for children in specific process group */
                pid_t pgid = -pid;
                child = find_zombie_child_in_pgid(parent->pid, pgid);
                if (!child) {
                        pr_debug("[wait4] No zombie children found in pgid %d\n",
                                 pgid);
                        return -LINUX_ECHILD;
                }

                child_pa = linux_proc_append(child);
                if (!child_pa) {
                        pr_error("[wait4] Child has no proc_append\n");
                        return -LINUX_ESRCH;
                }

                child_pid = child->pid;
        }

        /* Check if child already exited (zombie) */
        if (child_pa->exit_state == 1) {
                i32 exit_code = child_pa->exit_code;

                pr_debug("[wait4] Child PID=%d already exited (zombie)\n",
                         child_pid);

                if (wstatus) {
                        *wstatus = exit_code;
                }

                /* Mark child as reaped */
                child_pa->exit_state = 2; /* 2 = reaped */

                return (i64)child_pid;
        }

        /* Handle WNOHANG: non-blocking check */
        if (options & LINUX_WNOHANG) {
                pr_debug("[wait4] WNOHANG: child still running\n");
                return 0;
        }

        /* For pid > 0, use IPC blocking wait */
        if (pid > 0) {
                /* Create wait port and block for exit message */
                char port_name[WAIT_PORT_NAME_MAX];
                make_wait_port_name(port_name, sizeof(port_name), parent->pid);

                Message_Port_t* wait_port = thread_lookup_port(port_name);
                if (!wait_port) {
                        pr_debug("[wait4] Creating wait port '%s'\n", port_name);
                        wait_port = create_message_port(port_name);
                        if (!wait_port) {
                                pr_error("[wait4] Failed to create wait port\n");
                                return -LINUX_EAGAIN;
                        }
                        /* Register the port in the global port table so child can find it */
                        error_t reg_e = register_port(global_port_table, wait_port);
                        if (reg_e != REND_SUCCESS) {
                                pr_warn("[wait4] Failed to register wait port '%s': %d\n",
                                        port_name, (int)reg_e);
                        } else {
                                pr_debug("[wait4] Created and registered wait port '%s'\n",
                                         port_name);
                        }
                } else {
                        pr_debug("[wait4] Using existing wait port '%s'\n", port_name);
                }

                pr_debug("[wait4] PID=%d waiting on port '%s' for child PID=%d\n",
                         parent->pid, port_name, child_pid);

                /* Block waiting for exit message from child */
                error_t recv_e = recv_msg(wait_port);
                if (recv_e != REND_SUCCESS) {
                        pr_error("[wait4] Failed to receive message (e=%d)\n", (int)recv_e);
                        return -LINUX_EINTR;
                }

                Message_t* msg = dequeue_recv_msg();
                if (!msg) {
                        pr_error("[wait4] Failed to dequeue message\n");
                        return -LINUX_EINTR;
                }

                /* Extract exit information from message data */
                const kmsg_t* kmsg = kmsg_from_msg(msg);
                if (!kmsg || kmsg->hdr.magic != KMSG_MAGIC) {
                        pr_error("[wait4] Invalid kmsg magic\n");
                        dequeue_recv_msg();
                        return -LINUX_ECHILD;
                }

                /* Verify module and opcode */
                if (kmsg->hdr.module != KMSG_MOD_LINUX_COMPAT
                    || kmsg->hdr.opcode != KMSG_LINUX_EXIT_NOTIFY) {
                        pr_error("[wait4] Invalid kmsg module/opcode (mod=%u, op=%u)\n",
                                 kmsg->hdr.module, kmsg->hdr.opcode);
                        dequeue_recv_msg();
                        return -LINUX_ECHILD;
                }

                /* Decode the payload: format "qi" = i64(pid) + i32(exit_code) */
                i64 child_pid_i64;
                i32 exit_code;
                error_t dec_e = ipc_serial_decode(kmsg->payload,
                                                  kmsg->hdr.payload_len,
                                                  "qi",
                                                  &child_pid_i64,
                                                  &exit_code);
                if (dec_e != REND_SUCCESS) {
                        pr_error("[wait4] Failed to decode payload (e=%d)\n", (int)dec_e);
                        dequeue_recv_msg();
                        return -LINUX_ECHILD;
                }

                dequeue_recv_msg();

                pr_debug("[wait4] Received exit notification: child_pid=%d, exit_code=%d\n",
                         (pid_t)child_pid_i64, exit_code);

                /* Copy exit status to user space */
                if (wstatus) {
                        *wstatus = exit_code;
                }

                return (i64)child_pid_i64;
        } else {
                /* For pid == -1, 0, or < -1, we should have found a zombie above */
                pr_error("[wait4] Unexpected: non-zombie child for pid=%d\n", pid);
                return -LINUX_ECHILD;
        }
}
