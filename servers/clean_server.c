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
#include <linux_compat/ipc/clean_protocol.h>
#include <linux_compat/ipc/rpc.h>
#include <linux_compat/initcall.h>
#include <linux_compat/test_sync_ipc.h>
#ifdef LINUX_COMPAT_TEST
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/test_runner.h>
#endif

extern struct Port_Table *global_port_table;

/* Per-CPU clean server thread pointers */
DEFINE_PER_CPU(Thread_Base *, clean_server_thread_ptr);

static char clean_server_thread_name[] = "clean_server_thread";

static u16 clean_server_service_id;
static bool clean_server_port_registered;

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

        if (!km || km->hdr.module != clean_server_service_id
            || km->hdr.opcode != KMSG_OP_CLEAN_THREAD_REAP) {
                pr_error("[clean_server] Invalid kmsg\n");
                return;
        }

        void *vthread;
        i64 exit_code;
        if (ipc_serial_decode(km->payload,
                              km->hdr.payload_len,
                              LINUX_KMSG_FMT_THREAD_REAP,
                              &vthread,
                              &exit_code)
            != REND_SUCCESS) {
                pr_error("[clean_server] Failed to decode kmsg payload\n");
                return;
        }

        Thread_Base *target = (Thread_Base *)vthread;
        if (!target) {
                pr_error("[clean_server] NULL target thread\n");
                return;
        }

        Thread_Base *curr = get_cpu_current_thread();
        if (target == curr) {
                pr_error("[clean_server] Cannot cleanup current thread\n");
                return;
        }

        if (target == percpu(init_thread_ptr)
            || target == percpu(idle_thread_ptr)) {
                pr_error("[clean_server] Cannot cleanup init/idle thread\n");
                return;
        }

        /* Wait for owner CPU to mark ZOMBIE (exit intent is THREAD_FLAG_…
         * only). */
        if (target->flags & THREAD_FLAG_EXIT_REQUESTED) {
                while (thread_get_status(target) != thread_status_zombie) {
                        schedule(percpu(core_tm));
                }
        }

        u64 status = thread_get_status(target);
        if (status != thread_status_zombie) {
                pr_error(
                        "[clean_server] Thread not in zombie status (status=%lu)\n",
                        status);
                return;
        }

#ifdef LINUX_COMPAT_TEST
        /* Notify linux compat user test runner (if this was a test-managed user
         * thread). */
        linux_thread_append_t *ta = linux_thread_append(target);
        if (ta && ta->test_cookie != 0 && target->tm) {
                linux_user_test_notify_exit(
                        (i32)target->tm->owner_cpu, ta->test_cookie, exit_code);
        }
#endif

        Tcb_Base *task = target->belong_tcb;
        delete_thread(target);

        bool task_empty = false;
        if (task) {
                lock_cas(&task->thread_list_lock);
                task_empty = (task->thread_number == 0);
                unlock_cas(&task->thread_list_lock);
        }
        if (task && task_empty) {
                /*
                 * Check if this is a Linux compat task that is still zombie.
                 * exit_state: 0=running, 1=zombie, 2=reaped
                 *
                 * If exit_state == 1 (zombie):
                 *   - Check if parent task still exists (not just wait_port)
                 *   - If parent exists: don't delete (wait4 will reap)
                 *   - If parent doesn't exist: delete (orphan)
                 *
                 * If exit_state == 2 (reaped): safe to delete
                 */
                bool should_delete = true;
#ifdef LINUX_COMPAT_TEST
                linux_proc_append_t *pa = linux_proc_append(task);
                if (pa && pa->exit_state == 1) {
                        if (pa->ppid > 0) {
                                Tcb_Base *parent_task =
                                        find_task_by_pid(pa->ppid);
                                if (parent_task)
                                        should_delete = false;
                        }
                }
                if (pa && pa->exit_state == 2)
                        should_delete = true;
#endif

                if (should_delete) {
                        /*
                         * Active refcount teardown is expected to be safe:
                         * `schedule()` switches hardware roots (CR3/TTBR0) on
                         * context switches, so by the time this task is empty
                         * (all threads removed and thread marked ZOMBIE), this
                         * CPU should have dropped its active ref to the user
                         * vspace. Other CPUs follow their own schedule switches
                         * and the vspace is freed only on the last ref.
                         */
                        if (task->vs == &root_vspace) {
                                pr_error(
                                        "[ Error ] a user task should not use root vspace as its vspace\n");
                        }

#ifdef LINUX_COMPAT_TEST
                        /* Unregister from proc_registry before deleting */
                        linux_proc_append_t *pa = linux_proc_append(task);
                        if (pa)
                                unregister_process(task);
#endif

                        error_t e = delete_task(task);
                        if (e != REND_SUCCESS) {
                                pr_error(
                                        "[ Error ] delete_task failed (task=%p, e=%d)\n",
                                        (void *)task,
                                        e);
                        }
                }
        }
}

static void clean_server_on_message(Message_t *msg, u16 service_id)
{
        (void)service_id;
        clean_handle_message(msg);
}

void clean_server_thread(void)
{
        pr_info("[clean_server] CPU %lu: entering ipc_server_recv_loop on '%s'\n",
                (u64)percpu(cpu_number),
                CLEAN_SERVER_PORT_NAME);
        ipc_server_recv_loop(CLEAN_SERVER_PORT_NAME, clean_server_on_message);
}

static void clean_server_init(void)
{
        cpu_id_t cpu = percpu(cpu_number);

        pr_info("[clean_server] CPU %lu: Initializing clean_server\n",
                (u64)cpu);

        /* Global port: BSP once (AP initcalls run after BSP bootstrap). */
        if (linux_init_bsp_once(&clean_server_port_registered)) {
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
                linux_init_bsp_mark_done(&clean_server_port_registered);
        }

        if (clean_server_service_id == 0) {
                pr_info("[clean_server] CPU %lu: Looking up clean_server_port\n",
                        (u64)cpu);
                Message_Port_t *p = thread_lookup_port(CLEAN_SERVER_PORT_NAME);
                if (p) {
                        clean_server_service_id = p->service_id;
                        pr_info("[clean_server] CPU %lu: Found service_id=%u\n",
                                (u64)cpu,
                                clean_server_service_id);
                        ref_put(&p->refcount, free_message_port_ref);
                } else {
                        pr_error(
                                "[clean_server] CPU %lu: Failed to find clean_server_port\n",
                                (u64)cpu);
                }
        }

        /* Per-CPU recv thread (each core drains its own IPC queue). */
        pr_info("[clean_server] CPU %lu: Creating clean_server_thread\n",
                (u64)cpu);
        error_t e = gen_thread_from_func(&percpu(clean_server_thread_ptr),
                                         (kthread_func)clean_server_thread,
                                         clean_server_thread_name,
                                         percpu(core_tm),
                                         NULL);
        if (e != REND_SUCCESS) {
                pr_error("[ Error ]clean server init fail (e=%d)\n", (int)e);
        } else {
                pr_info("[clean_server] CPU %lu: clean_server_thread created successfully\n",
                        (u64)cpu);
        }
}
DEFINE_INIT(clean_server_init);