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

#define CLEAN_SERVER_PORT_NAME     "clean_server_port"
#define CLEAN_KMSG_FMT_THREAD_REAP LINUX_KMSG_FMT_THREAD_REAP

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
         */
        if (task) {
                linux_proc_append_t* pa = linux_proc_append(task);
                if (pa) {
                        pa->exit_code = (i32)exit_code;
                        pa->exit_state = 1; /* 1 = zombie */
                        pr_info("[sys_exit] Task PID=%d exit_code=%d exit_state=%d\n",
                                task->pid, pa->exit_code, pa->exit_state);
                }
        }

        /*
         * Exit intent: THREAD_FLAG_EXIT_REQUESTED (survives IPC status
         * changes). Owner CPU scheduler moves running -> zombie on switch-away.
         */
        thread_or_flags(self, THREAD_FLAG_EXIT_REQUESTED);

        pr_info(
                "[sys_exit] CPU %lu: thread %s (tid=%lu) requesting exit with code %ld\n",
                (u64)percpu(cpu_number),
                self->name ? self->name : "(unnamed)",
                (u64)self->tid,
                exit_code);

        pr_debug("[sys_exit] CPU %lu: Looking up clean_server port '%s'\n",
                 (u64)percpu(cpu_number), CLEAN_SERVER_PORT_NAME);

        port = thread_lookup_port(CLEAN_SERVER_PORT_NAME);
        if (!port) {
                pr_error("[sys_exit] CPU %lu: port %s not found\n",
                         (u64)percpu(cpu_number), CLEAN_SERVER_PORT_NAME);
                goto out;
        }

        pr_debug("[sys_exit] CPU %lu: Port found (service_id=%u)\n",
                 (u64)percpu(cpu_number), port->service_id);

        /*
         * Optional ACK back to linux compat test runner:
         * - reply port name is per-CPU, registered by user_test_runner
         * - cookie comes from linux_thread_append(self)->test_cookie
         */
        char reply_name[64];
        reply_name[0] = '\0';
        u64 cookie = 0;
#ifdef LINUX_COMPAT_TEST
        linux_thread_append_t* ta = linux_thread_append(self);
        if (ta && ta->test_cookie != 0) {
                cookie = ta->test_cookie;
                (void)linux_exit_ack_port_name(
                        reply_name, (u32)sizeof(reply_name), (u64)percpu(cpu_number));
                pr_debug("[sys_exit] CPU %lu: Test cookie=0x%lx, reply_port='%s'\n",
                         (u64)percpu(cpu_number), cookie, reply_name);
        }
#endif

        pr_debug("[sys_exit] CPU %lu: Creating kmsg for clean_server\n",
                 (u64)percpu(cpu_number));

        md = kmsg_create(port->service_id,
                        KMSG_OP_CORE_THREAD_REAP,
                        CLEAN_KMSG_FMT_THREAD_REAP,
                        self,
                        exit_code,
                        (reply_name[0] != '\0') ? reply_name : (char*)NULL,
                        (i64)cookie);
        if (!md) {
                pr_error("[sys_exit] CPU %lu: kmsg_create failed\n",
                         (u64)percpu(cpu_number));
                goto out_put_port;
        }

        pr_debug("[sys_exit] CPU %lu: Creating message from kmsg\n",
                 (u64)percpu(cpu_number));

        msg = create_message_with_msg(md);
        ref_put(&md->refcount, md->free_data);
        if (!msg) {
                pr_error("[sys_exit] CPU %lu: create_message_with_msg failed\n",
                         (u64)percpu(cpu_number));
                goto out_put_port;
        }

        pr_debug("[sys_exit] CPU %lu: Enqueuing message for send\n",
                 (u64)percpu(cpu_number));

        error_t ie = enqueue_msg_for_send(msg);
        if (ie != REND_SUCCESS) {
                pr_error("[sys_exit] CPU %lu: enqueue_msg_for_send failed e=%d\n",
                         (u64)percpu(cpu_number), (int)ie);
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                goto out_put_port;
        }

        pr_info("[sys_exit] CPU %lu: Sending message to clean_server\n",
                (u64)percpu(cpu_number));

        (void)send_msg(port);

        pr_info("[sys_exit] CPU %lu: Message sent, calling schedule to become zombie\n",
                (u64)percpu(cpu_number));

        /*
         * Message successfully enqueued and sent.
         * The queue now owns the message reference; we release our claim.
         */
        msg = NULL;
out_put_port:
        if (port) {
                ref_put(&port->refcount, free_message_port_ref);
        }
out:
        /*
         * `exit(2)` must not return to user mode.
         *
         * The core scheduler will transition this thread from RUNNING to ZOMBIE
         * on the next schedule() call because:
         * 1. We set THREAD_FLAG_EXIT_REQUESTED above
         * 2. The thread is still in thread_status_running state
         * 3. The scheduler checks for EXIT_REQUESTED flag and transitions to zombie
         *
         * We call schedule() to trigger this transition, then park forever.
         */
        schedule(percpu(core_tm));

        /*
         * We should never reach here - the thread should be marked as zombie
         * by the scheduler and will be reaped by clean_server.
         * If we somehow run again, park in suspend to prevent return to user mode.
         */
        if (self) {
                (void)thread_set_status(self, thread_status_suspend);
        }
        for (;;)
                ;
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
                char reply_name[64];
                reply_name[0] = '\0';
                u64 cookie = 0;
#ifdef LINUX_COMPAT_TEST
                linux_thread_append_t* ta = linux_thread_append(self);
                if (ta && ta->test_cookie != 0) {
                        cookie = ta->test_cookie;
                        (void)linux_exit_ack_port_name(reply_name,
                                                       (u32)sizeof(reply_name),
                                                       (u64)percpu(cpu_number));
                }
#endif

                md = kmsg_create(port->service_id,
                                 KMSG_OP_CORE_THREAD_REAP,
                                 CLEAN_KMSG_FMT_THREAD_REAP,
                                 self,
                                 exit_code,
                                 (reply_name[0] != '\0') ? reply_name
                                                         : (char*)NULL,
                                 (i64)cookie);
                if (md) {
                        msg = create_message_with_msg(md);
                        ref_put(&md->refcount, md->free_data);
                        if (msg) {
                                if (enqueue_msg_for_send(msg) == REND_SUCCESS) {
                                        (void)send_msg(port);
                                        /* Queue owns the message now; we release our claim */
                                        msg = NULL;
                                } else {
                                        /* Enqueue failed; release our reference */
                                        ref_put(&msg->ms_queue_node.refcount,
                                                free_message_ref);
                                }
                        }
                }
                ref_put(&port->refcount, free_message_port_ref);
        }

        /*
         * Schedule to trigger transition to zombie status.
         * The core scheduler will handle this because we set EXIT_REQUESTED flag.
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
        for (;;)
                schedule(percpu(core_tm));
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
        list_for_each_safe(pos, next, &task->thread_head_node) {
                Thread_Base* thread = container_of(pos, Thread_Base, thread_list_node);

                /* Skip current thread - we kill it last */
                if (thread == get_cpu_current_thread()) {
                        continue;
                }

                /* Set exit flag for this thread */
                thread_or_flags(thread, THREAD_FLAG_EXIT_REQUESTED);

                pr_debug("[exit_group] Killing thread %s (tid=%lu) in task PID=%d\n",
                         thread->name ? thread->name : "(unnamed)",
                         (u64)thread->tid,
                         task->pid);
        }

        unlock_cas(&task->thread_list_lock);

        /* Finally kill current thread */
        pr_info("[exit_group] Exiting task PID=%d with code %ld\n",
                task->pid, exit_code);
        sys_exit(exit_code);
}
