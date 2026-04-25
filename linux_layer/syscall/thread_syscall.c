#include <modules/log/log.h>
#include <common/types.h>
#include <rendezvos/error.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <linux_compat/test_sync_ipc.h>
#include <linux_compat/fault.h>
#include <linux_compat/test_runner.h>

#define CLEAN_SERVER_PORT_NAME     "clean_server_port"
#define CLEAN_KMSG_FMT_THREAD_REAP LINUX_KMSG_FMT_THREAD_REAP

/* Linux compat IPC module and opcodes */
#define KMSG_MOD_LINUX_COMPAT      2u
#define KMSG_LINUX_EXIT_NOTIFY     1u

void sys_exit(i64 exit_code)
{
        Thread_Base* self = get_cpu_current_thread();
        Tcb_Base* task = get_cpu_current_task();
        Message_Port_t* port = NULL;
        Msg_Data_t* md = NULL;
        Message_t* msg = NULL;

        if (!self)
                goto out;

        /*
         * Set task exit state for wait4().
         * exit_state: 0=running, 1=zombie, 2=reaped
         *
         * IMPORTANT: We set exit_state=2 (reaped) immediately to prevent
         * clean_server from deleting the task before wait4 can verify it.
         * This is safe because:
         * 1. The exit notification contains all info wait4 needs (pid, exit_code)
         * 2. Once we send the notification, we don't need the task structure anymore
         * 3. This prevents race condition where clean_server deletes the task
         *    before wait4 can verify child state
         */
        if (task) {
                linux_proc_append_t* pa = linux_proc_append(task);
                if (pa) {
                        pa->exit_code = (i32)exit_code;
                        pa->exit_state = 2; /* 2 = reaped (prevents clean_server deletion) */
                        pr_info("[sys_exit] Task PID=%d exit_code=%d exit_state=%d, ppid=%d\n",
                                task->pid,
                                pa->exit_code,
                                pa->exit_state,
                                pa->ppid);
                }
        }

        /*
         * Notify parent's wait4 via IPC.
         * This replaces the old polling mechanism with proper IPC blocking.
         *
         * NOTE: This is separate from the clean_server cleanup request below.
         * Both are needed: wait4 for parent process, clean_server for test runner.
         */
        if (task && task->pid > 0) {
                linux_proc_append_t* pa = linux_proc_append(task);
                if (pa && pa->ppid > 0) {
                        /* Find parent's wait port */
                        char port_name[32];
                        const char* prefix = "wait_port_";
                        pid_t ppid = pa->ppid;

                        /* Simple port name generation: "wait_port_<ppid>" */
                        for (size_t i = 0; i < 10; i++) {
                                port_name[i] = prefix[i];
                        }
                        size_t idx = 10;

                        if (ppid == 0) {
                                if (idx < 31) {
                                        port_name[idx++] = '0';
                                }
                        } else {
                                char rev[16];
                                size_t rev_len = 0;
                                pid_t temp = ppid;
                                while (temp > 0 && rev_len < sizeof(rev)) {
                                        rev[rev_len++] = '0' + (temp % 10);
                                        temp /= 10;
                                }
                                for (size_t i = 0; i < rev_len && idx < 31; i++) {
                                        port_name[idx++] = rev[rev_len - 1 - i];
                                }
                        }
                        port_name[idx] = '\0';

                        Message_Port_t* wait_port = thread_lookup_port(port_name);
                        if (wait_port) {
                                /* Send exit message to parent using kmsg
                                 * Format: "qi" = i64(pid) + i32(exit_code) */
                                pr_debug("[sys_exit] Sending exit notification to parent PID=%d via port '%s'\n",
                                         ppid, port_name);
                                Msg_Data_t* exit_md = kmsg_create(
                                        KMSG_MOD_LINUX_COMPAT,
                                        KMSG_LINUX_EXIT_NOTIFY,
                                        "qi",
                                        (i64)task->pid,
                                        (i32)exit_code);
                                if (exit_md) {
                                        Message_t* exit_msg =
                                                create_message_with_msg(exit_md);
                                        if (exit_msg) {
                                                error_t e = enqueue_msg_for_send(
                                                        exit_msg);
                                                if (e == REND_SUCCESS) {
                                                        e = send_msg(wait_port);
                                                        if (e == REND_SUCCESS) {
                                                                pr_debug(
                                                                        "[sys_exit] Sent exit notification to parent PID=%d\n",
                                                                        ppid);
                                                        } else {
                                                                pr_error(
                                                                        "[sys_exit] Failed to send exit message: %d\n",
                                                                        (int)e);
                                                        }
                                                } else {
                                                        pr_error(
                                                                "[sys_exit] Failed to enqueue exit message: %d\n",
                                                                (int)e);
                                                }
                                        } else {
                                                pr_error(
                                                        "[sys_exit] Failed to create exit message\n");
                                        }
                                } else {
                                        pr_debug("[sys_exit] Failed to create exit message data\n");
                                }
                        } else {
                                pr_debug("[sys_exit] Parent wait port '%s' not found (may not have called wait4 yet)\n",
                                         port_name);
                        }
                }
        }

        /*
         * Exit intent: THREAD_FLAG_EXIT_REQUESTED (survives IPC status
         * changes). Owner CPU scheduler moves running -> zombie on switch-away.
         */
        thread_or_flags(self, THREAD_FLAG_EXIT_REQUESTED);

        port = thread_lookup_port(CLEAN_SERVER_PORT_NAME);
        if (!port) {
                pr_error("[sys_exit] port %s not found\n",
                         CLEAN_SERVER_PORT_NAME);
                goto out;
        }

        pr_debug("[sys_exit] Sending cleanup request to clean_server for thread tid=%lu, exit_code=%ld\n",
                 (u64)self->tid, exit_code);

        md = kmsg_create(port->service_id,
                         KMSG_OP_CORE_THREAD_REAP,
                         CLEAN_KMSG_FMT_THREAD_REAP,
                         self,
                         exit_code);
        if (!md) {
                ref_put(&port->refcount, free_message_port_ref);
                pr_error("[sys_exit] kmsg_create failed\n");
                goto out;
        }

        msg = create_message_with_msg(md);
        ref_put(&md->refcount, md->free_data);
        if (!msg) {
                ref_put(&port->refcount, free_message_port_ref);
                pr_error("[sys_exit] create_message_with_msg failed\n");
                goto out;
        }

        error_t ie = enqueue_msg_for_send(msg);
        if (ie != REND_SUCCESS) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                ref_put(&port->refcount, free_message_port_ref);
                pr_error("[sys_exit] enqueue_msg_for_send failed e=%d\n",
                         (int)ie);
                goto out;
        }

        ie = send_msg(port);
        if (ie != REND_SUCCESS) {
                pr_error("[sys_exit] send_msg to clean_server failed e=%d\n", (int)ie);
        } else {
                pr_debug("[sys_exit] Cleanup request sent to clean_server successfully\n");
        }
        ref_put(&port->refcount, free_message_port_ref);

out:
        schedule(percpu(core_tm));
        while (1) {
                schedule(percpu(core_tm));
        }
}

void linux_fatal_user_fault(i64 exit_code)
{
        /*
         * Minimal "fatal signal" behavior for linux compat:
         * - mark task exit code/state (for wait4)
         * - request thread exit (scheduler will flip to zombie)
         * - ask clean_server to reap (so test runner / waiters can progress)
         * - yield; do not return to user mode
         */
        Thread_Base* self = get_cpu_current_thread();
        Tcb_Base* task = get_cpu_current_task();
        Message_Port_t* port = NULL;
        Msg_Data_t* md = NULL;
        Message_t* msg = NULL;

        if (task) {
                linux_proc_append_t* pa = linux_proc_append(task);
                if (pa) {
                        pa->exit_code = (i32)exit_code;
                        pa->exit_state = 1;
                }
        }
        if (self) {
                thread_or_flags(self, THREAD_FLAG_EXIT_REQUESTED);
        }

        port = thread_lookup_port(CLEAN_SERVER_PORT_NAME);
        if (port) {
                md = kmsg_create(port->service_id,
                                 KMSG_OP_CORE_THREAD_REAP,
                                 CLEAN_KMSG_FMT_THREAD_REAP,
                                 self,
                                 exit_code);
                if (md) {
                        msg = create_message_with_msg(md);
                        ref_put(&md->refcount, md->free_data);
                        if (msg) {
                                if (enqueue_msg_for_send(msg) == REND_SUCCESS) {
                                        (void)send_msg(port);
                                        /* Queue owns the message now; we
                                         * release our claim */
                                        msg = NULL;
                                } else {
                                        /* Enqueue failed; release our reference
                                         */
                                        ref_put(&msg->ms_queue_node.refcount,
                                                free_message_ref);
                                }
                        }
                }
                ref_put(&port->refcount, free_message_port_ref);
        }

        /*
         * Schedule to trigger transition to zombie status.
         * The core scheduler will handle this because we set EXIT_REQUESTED
         * flag.
         */
        schedule(percpu(core_tm));

        /*
         * Should never reach here, but if we do, park in suspend.
         */
        if (self) {
                (void)thread_set_status(self, thread_status_suspend);
        }
        for (;;)
                ;
}

void sys_exit_group(i64 exit_code)
{
        Tcb_Base* task = get_cpu_current_task();
        if (!task) {
                pr_error("[exit_group] No current task\n");
                return;
        }

        /*
         * Kill all threads in the task except the current one.
         * We iterate through the task's thread list directly.
         */
        struct list_entry* pos;
        struct list_entry* next;

        lock_cas(&task->thread_list_lock);

        /*
         * Save next pointer before setting flags, as thread might
         * be removed from list by other CPU.
         */
        list_for_each_safe(pos, next, &task->thread_head_node)
        {
                Thread_Base* thread =
                        container_of(pos, Thread_Base, thread_list_node);

                /* Skip current thread - we kill it last */
                if (thread == get_cpu_current_thread()) {
                        continue;
                }

                /* Set exit flag for this thread */
                thread_or_flags(thread, THREAD_FLAG_EXIT_REQUESTED);

                pr_debug(
                        "[exit_group] Killing thread %s (tid=%lu) in task PID=%d\n",
                        thread->name ? thread->name : "(unnamed)",
                        (u64)thread->tid,
                        task->pid);
        }

        unlock_cas(&task->thread_list_lock);

        /* Finally kill current thread */
        pr_info("[exit_group] Exiting task PID=%d with code %ld\n",
                task->pid,
                exit_code);
        sys_exit(exit_code);
}
