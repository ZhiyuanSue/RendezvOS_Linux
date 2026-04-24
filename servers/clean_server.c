#include <modules/log/log.h>
#include <common/types.h>
#include <rendezvos/ipc/ipc_serial.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/thread_loader.h>
#include <linux_compat/test_sync_ipc.h>
#ifdef LINUX_COMPAT_TEST
#include <linux_compat/proc_compat.h>
#include <linux_compat/test_runner.h>
#endif

/* Per-CPU clean server thread pointers */
DEFINE_PER_CPU(Thread_Base *, clean_server_thread_ptr);

static char clean_server_thread_name[] = "clean_server_thread";
#define CLEAN_SERVER_PORT_NAME     "clean_server_port"
#define CLEAN_KMSG_FMT_THREAD_REAP LINUX_KMSG_FMT_THREAD_REAP

static u16 clean_server_service_id;

static void clean_handle_message(Message_t *msg)
{
        if (!msg || !msg->data) {
                pr_error("[clean_server] NULL msg or msg->data\n");
                return;
        }

        const kmsg_t *km = kmsg_from_msg(msg);
        if (!km) {
                pr_error("[clean_server] NULL kmsg\n");
                return;
        }

        pr_debug("[clean_server] CPU %lu: Received kmsg: module=%u, opcode=%u, service_id=%u\n",
                 (u64)percpu(cpu_number),
                 km->hdr.module,
                 km->hdr.opcode,
                 clean_server_service_id);

        if (!km || km->hdr.module != clean_server_service_id
            || km->hdr.opcode != KMSG_OP_CORE_THREAD_REAP) {
                pr_error("[clean_server] CPU %lu: Invalid kmsg: module=%u (expected %u), opcode=%u (expected %u)\n",
                         (u64)percpu(cpu_number),
                         km->hdr.module,
                         clean_server_service_id,
                         km->hdr.opcode,
                         (u16)KMSG_OP_CORE_THREAD_REAP);
                return;
        }

        void *vthread;
        i64 exit_code;
        char *reply_port_name;
        i64 cookie;
        if (ipc_serial_decode(km->payload,
                              km->hdr.payload_len,
                              CLEAN_KMSG_FMT_THREAD_REAP,
                              &vthread,
                              &exit_code,
                              &reply_port_name,
                              &cookie)
            != REND_SUCCESS) {
                pr_error("[clean_server] CPU %lu: Failed to decode kmsg payload\n",
                         (u64)percpu(cpu_number));
                return;
        }

        pr_info("[clean_server] CPU %lu: Decoded THREAD_REAP: thread=%p, exit_code=%ld, reply_port='%s', cookie=0x%lx\n",
                (u64)percpu(cpu_number),
                vthread,
                exit_code,
                reply_port_name ? reply_port_name : "(null)",
                (u64)cookie);

        Thread_Base *target = (Thread_Base *)vthread;
        if (!target) {
                pr_error("[clean_server] NULL target thread\n");
                return;
        }
        pr_debug("[clean_server] CPU %lu: target thread=%p, tid=%lu, name='%s'\n",
                 (u64)percpu(cpu_number),
                 target,
                 (u64)target->tid,
                 target->name ? target->name : "(unnamed)");

        Thread_Base *curr = get_cpu_current_thread();
        pr_debug("[clean_server] CPU %lu: curr thread=%p, tid=%lu, name='%s'\n",
                 (u64)percpu(cpu_number),
                 curr,
                 (u64)curr->tid,
                 curr->name ? curr->name : "(unnamed)");

        if (target == curr) {
                pr_error("[clean_server] Cannot cleanup current thread\n");
                return;
        }

        pr_debug("[clean_server] CPU %lu: Checking init_thread_ptr=%p, idle_thread_ptr=%p\n",
                 (u64)percpu(cpu_number),
                 percpu(init_thread_ptr),
                 percpu(idle_thread_ptr));

        if (target == percpu(init_thread_ptr)
            || target == percpu(idle_thread_ptr)) {
                pr_error("[clean_server] Cannot cleanup init/idle thread\n");
                return;
        }

        pr_debug("[clean_server] CPU %lu: Checking thread flags: 0x%lx\n",
                 (u64)percpu(cpu_number),
                 target->flags);

        /* Wait for owner CPU to mark ZOMBIE (exit intent is THREAD_FLAG_…
         * only). */
        if (target->flags & THREAD_FLAG_EXIT_REQUESTED) {
                pr_debug("[clean_server] CPU %lu: Thread has EXIT_REQUESTED flag, waiting for zombie status\n",
                         (u64)percpu(cpu_number));
                while (thread_get_status(target) != thread_status_zombie) {
                        schedule(percpu(core_tm));
                }
        }

        u64 status = thread_get_status(target);
        pr_debug("[clean_server] CPU %lu: Thread status=%lu, expected=%d (zombie)\n",
                 (u64)percpu(cpu_number),
                 status,
                 thread_status_zombie);

        if (status != thread_status_zombie) {
                pr_error("[clean_server] Thread not in zombie status (status=%lu)\n", status);
                return;
        }

        pr_debug("[clean_server] CPU %lu: All checks passed, proceeding with cleanup\n",
                 (u64)percpu(cpu_number));

        /* Debug output: show which thread is being cleaned up on which core */
        pr_info(
                "[clean_server] CPU %lu: Cleaning up thread %s (tid=%lu, exit_code=%ld)\n",
                (u64)percpu(cpu_number),
                target->name ? target->name : "(unnamed)",
                (u64)target->tid,
                exit_code);

#ifdef LINUX_COMPAT_TEST
        /* Notify linux compat user test runner (if this was a test-managed user
         * thread). */
        linux_thread_append_t *ta = linux_thread_append(target);
        if (ta && ta->test_cookie != 0 && target->tm) {
                pr_info("[clean_server] CPU %lu: Notifying linux_user_test_notify_exit: owner_cpu=%d, cookie=0x%lx, exit_code=%ld\n",
                        (u64)percpu(cpu_number),
                        (i32)target->tm->owner_cpu,
                        ta->test_cookie,
                        exit_code);
                linux_user_test_notify_exit(
                        (i32)target->tm->owner_cpu, ta->test_cookie, exit_code);
        } else {
                pr_debug("[clean_server] CPU %lu: Not a test thread (ta=%p, cookie=0x%lx, tm=%p)\n",
                         (u64)percpu(cpu_number),
                         ta,
                         ta ? ta->test_cookie : 0,
                         target->tm);
        }
#endif

        /*
         * Optional synchronous ACK: if the caller provided a reply port name and
         * cookie, send an ACK message after the thread is confirmed zombie.
         */
        if (reply_port_name && cookie != 0) {
                pr_info("[clean_server] CPU %lu: Sending ACK to reply_port='%s', cookie=0x%lx\n",
                        (u64)percpu(cpu_number),
                        reply_port_name,
                        (u64)cookie);
                Message_Port_t *rp = thread_lookup_port(reply_port_name);
                if (rp) {
                        pr_debug("[clean_server] CPU %lu: Reply port found (service_id=%u)\n",
                                 (u64)percpu(cpu_number),
                                 rp->service_id);
                        Msg_Data_t *ack_md = kmsg_create(
                                rp->service_id,
                                (u16)KMSG_OP_CORE_THREAD_REAP_ACK,
                                LINUX_KMSG_FMT_EXIT_ACK,
                                (i64)cookie,
                                (i64)exit_code);
                        if (ack_md) {
                                Message_t *ack = create_message_with_msg(ack_md);
                                ref_put(&ack_md->refcount, ack_md->free_data);
                                if (ack) {
                                        if (enqueue_msg_for_send(ack) == REND_SUCCESS) {
                                                pr_debug("[clean_server] CPU %lu: ACK message enqueued, sending...\n",
                                                         (u64)percpu(cpu_number));
                                                (void)send_msg(rp);
                                                pr_debug("[clean_server] CPU %lu: ACK message sent, receiver marked READY by IPC\n",
                                                         (u64)percpu(cpu_number));

                                                /*
                                                 * Debug: Check if there's a ready thread that should run.
                                                 * After send_msg(), the test runner should be READY and
                                                 * should be scheduled before we block again.
                                                 */
                                                pr_debug("[clean_server] CPU %lu: About to loop back to recv_msg\n",
                                                         (u64)percpu(cpu_number));
                                        } else {
                                                pr_error("[clean_server] CPU %lu: Failed to enqueue ACK message\n",
                                                         (u64)percpu(cpu_number));
                                        }
                                        ref_put(&ack->ms_queue_node.refcount,
                                                free_message_ref);
                                } else {
                                        pr_error("[clean_server] CPU %lu: Failed to create ACK message\n",
                                                 (u64)percpu(cpu_number));
                                }
                        } else {
                                pr_error("[clean_server] CPU %lu: Failed to create ACK kmsg\n",
                                         (u64)percpu(cpu_number));
                        }
                        ref_put(&rp->refcount, free_message_port_ref);
                } else {
                        pr_error("[clean_server] CPU %lu: Failed to find reply port '%s'\n",
                                 (u64)percpu(cpu_number),
                                 reply_port_name);
                }
        } else {
                pr_debug("[clean_server] CPU %lu: No reply_port/cookie, skipping ACK\n",
                         (u64)percpu(cpu_number));
        }

        pr_debug("[clean_server] CPU %lu: ACK handling complete, proceeding with thread deletion\n",
                 (u64)percpu(cpu_number));

        Tcb_Base *task = target->belong_tcb;
        pr_debug("[clean_server] CPU %lu: Calling delete_thread for tid=%lu\n",
                 (u64)percpu(cpu_number), (u64)target->tid);

        delete_thread(target);

        pr_debug("[clean_server] CPU %lu: delete_thread returned, continuing with task cleanup\n",
                 (u64)percpu(cpu_number));

        bool task_empty = false;
        if (task) {
                lock_cas(&task->thread_list_lock);
                task_empty = (task->thread_number == 0);
                unlock_cas(&task->thread_list_lock);
        }
        if (task && task_empty) {
                /*
                 * Active refcount teardown is expected to be safe:
                 * `schedule()` switches hardware roots (CR3/TTBR0) on context
                 * switches, so by the time this task is empty (all threads
                 * removed and thread marked ZOMBIE), this CPU should have
                 * dropped its active ref to the user vspace. Other CPUs
                 * follow their own schedule switches and the vspace is
                 * freed only on the last ref.
                 */
                if (task->vs == &root_vspace) {
                        pr_error(
                                "[ Error ] a user task should not use root vspace as its vspace\n");
                }
                pr_info("[clean_server] CPU %lu: Task %p is empty, deleting task\n",
                        (u64)percpu(cpu_number),
                        (void*)task);
                pr_debug("[clean_server] CPU %lu: Calling delete_task for task=%p\n",
                         (u64)percpu(cpu_number), (void*)task);
                error_t e = delete_task(task);
                if (e) {
                        pr_error(
                                "[ Error ] delete_task failed (task=%p, e=%d)\n",
                                (void *)task,
                                e);
                }
                pr_debug("[clean_server] CPU %lu: delete_task returned\n",
                         (u64)percpu(cpu_number));
        }

        pr_debug("[clean_server] CPU %lu: Message handling complete, returning to message loop\n",
                 (u64)percpu(cpu_number));

        pr_debug("[clean_server] CPU %lu: Message handling complete, returning to message loop\n",
                 (u64)percpu(cpu_number));
}

void clean_server_thread(void)
{
        Message_Port_t *port = NULL;

        pr_info("[clean_server] CPU %lu: clean_server_thread started, looking up port '%s'\n",
                (u64)percpu(cpu_number), CLEAN_SERVER_PORT_NAME);

        /* Lookup global clean server port via global table */
        while (!port) {
                port = thread_lookup_port(CLEAN_SERVER_PORT_NAME);
                if (!port) {
                        /* Port not yet registered, sleep and retry */
                        pr_debug("[clean_server] CPU %lu: port not ready, waiting...\n",
                                 (u64)percpu(cpu_number));
                        schedule(percpu(core_tm));
                        continue;
                }
        }

        pr_info("[clean_server] CPU %lu: Port found, entering message loop\n",
                (u64)percpu(cpu_number));

        /* We hold a reference to the port via thread_lookup_port */
        while (1) {
                error_t ret = recv_msg(port);
                if (ret) {
                        /* recv_msg failed, maybe port deleted?
                         * Release reference and re-lookup port.
                         */
                        pr_debug("[clean_server] CPU %lu: recv_msg failed (e=%d), re-lookup port\n",
                                 (u64)percpu(cpu_number), (int)ret);
                        ref_put(&port->refcount, free_message_port_ref);
                        port = NULL;
                        while (!port) {
                                port = thread_lookup_port(
                                        CLEAN_SERVER_PORT_NAME);
                                if (!port) {
                                        schedule(percpu(core_tm));
                                        continue;
                                }
                        }
                        continue;
                }

                pr_debug("[clean_server] CPU %lu: Message received, processing...\n",
                         (u64)percpu(cpu_number));

                /*
                 * After recv_msg returns, explicitly yield to allow any threads
                 * woken up by previous IPC messages to run before we process
                 * the current message. This prevents a race where we process
                 * messages too quickly and don't give woken threads a chance.
                 */
                pr_debug("[clean_server] CPU %lu: Yielding before message processing\n",
                         (u64)percpu(cpu_number));
                schedule(percpu(core_tm));
                pr_debug("[clean_server] CPU %lu: Returned from yield, processing message\n",
                         (u64)percpu(cpu_number));

                /*
                 * Process at most one message per recv_msg call.
                 * This ensures we yield back to recv_msg() after each message,
                 * allowing the scheduler to switch threads if needed.
                 */
                Message_t *msg = dequeue_recv_msg();
                if (msg) {
                        pr_debug("[clean_server] CPU %lu: Got message, calling clean_handle_message\n",
                                 (u64)percpu(cpu_number));
                        clean_handle_message(msg);
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);

                        /*
                         * After handling a message, explicitly yield to allow
                         * other READY threads (like the test runner) to run.
                         * This ensures that threads woken up by IPC messages
                         * get a chance to run before clean_server blocks again.
                         */
                        pr_debug("[clean_server] CPU %lu: Message handled, yielding to scheduler\n",
                                 (u64)percpu(cpu_number));
                        schedule(percpu(core_tm));
                        pr_debug("[clean_server] CPU %lu: Scheduler returned, message processing complete\n",
                                 (u64)percpu(cpu_number));
                }
        }

        /* Should never reach here, but for completeness: release port reference
         */
        if (port) {
                ref_put(&port->refcount, free_message_port_ref);
        }
}

extern cpu_id_t BSP_ID;

static void clean_server_init(void)
{
        cpu_id_t cpu = percpu(cpu_number);

        pr_info("[clean_server] CPU %lu: Initializing clean_server\n", (u64)cpu);

        /* On BSP: create and register global clean server port */
        if (cpu == BSP_ID) {
                pr_info("[clean_server] BSP: Creating global clean server port\n");
                if (!global_port_table) {
                        pr_error(
                                "[clean_server] global_port_table not initialized\n");
                        return;
                }
                Message_Port_t *port =
                        create_message_port(CLEAN_SERVER_PORT_NAME);
                if (!port) {
                        pr_error(
                                "[clean_server] failed to create message port\n");
                        return;
                }

                clean_server_service_id = port->service_id;
                pr_info("[clean_server] BSP: Port created with service_id=%u\n",
                        clean_server_service_id);

                error_t err = register_port(global_port_table, port);
                if (err) {
                        pr_error("[clean_server] failed to register port: %d\n",
                                 (int)err);
                        delete_message_port_structure(port);
                        return;
                }
                pr_info("[clean_server] BSP: Port registered successfully as '%s'\n",
                        CLEAN_SERVER_PORT_NAME);
        }

        if (clean_server_service_id == 0) {
                pr_info("[clean_server] CPU %lu: Looking up clean_server_port\n",
                        (u64)cpu);
                Message_Port_t *p = thread_lookup_port(CLEAN_SERVER_PORT_NAME);
                if (p) {
                        clean_server_service_id = p->service_id;
                        pr_info("[clean_server] CPU %lu: Found service_id=%u\n",
                                (u64)cpu, clean_server_service_id);
                        ref_put(&p->refcount, free_message_port_ref);
                } else {
                        pr_error("[clean_server] CPU %lu: Failed to find clean_server_port\n",
                                 (u64)cpu);
                }
        }

        /* On all CPUs (including BSP): create clean server thread */
        pr_info("[clean_server] CPU %lu: Creating clean_server_thread\n", (u64)cpu);
        error_t e = gen_thread_from_func(&percpu(clean_server_thread_ptr),
                                         (kthread_func)clean_server_thread,
                                         clean_server_thread_name,
                                         percpu(core_tm),
                                         NULL);
        if (e) {
                pr_error("[ Error ]clean server init fail (e=%d)\n", (int)e);
        } else {
                pr_info("[clean_server] CPU %lu: clean_server_thread created successfully\n",
                        (u64)cpu);
        }
}
DEFINE_INIT(clean_server_init);