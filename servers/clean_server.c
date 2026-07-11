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
#include <rendezvos/sync/cas_lock.h>
#include <linux_compat/ipc/clean_protocol.h>
#include <linux_compat/ipc/rpc.h>
#include <linux_compat/initcall.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/proc/wait_ipc.h>
#include <linux_compat/test_sync_ipc.h>
#ifdef LINUX_COMPAT_TEST
#include <linux_compat/test_runner.h>
#endif

extern struct Port_Table *global_port_table;

DEFINE_PER_CPU(Thread_Base *, clean_server_thread_ptr);

static char clean_server_thread_name[] = "clean_server_thread";

static u16 clean_server_service_id;
static bool clean_server_port_registered;

/*
 * THREAD_REAP: core delete_thread() only. Task shell stays until TASK_REAP.
 * Do not call delete_task() here.
 */
static void clean_handle_thread_reap(const kmsg_t *km)
{
        void *vthread;
        i64 exit_code;

        if (ipc_serial_decode(km->payload,
                              km->hdr.payload_len,
                              LINUX_KMSG_FMT_THREAD_REAP,
                              &vthread,
                              &exit_code)
            != REND_SUCCESS) {
                pr_error("[clean_server] THREAD_REAP: decode failed\n");
                return;
        }

        Thread_Base *target = (Thread_Base *)vthread;
        Tcb_Base *task = target ? target->belong_tcb : NULL;
        pid_t task_pid = task ? task->pid : 0;

        if (!target) {
                pr_error("[clean_server] THREAD_REAP: NULL thread\n");
                return;
        }

        Thread_Base *curr = get_cpu_current_thread();
        if (target == curr) {
                pr_error("[clean_server] THREAD_REAP: cannot reap current thread\n");
                return;
        }

        if (target == percpu(init_thread_ptr)
            || target == percpu(idle_thread_ptr)) {
                pr_error("[clean_server] THREAD_REAP: init/idle blocked\n");
                return;
        }

        if (target->flags & THREAD_FLAG_EXIT_REQUESTED) {
                while (thread_get_status(target) != thread_status_zombie) {
                        schedule(percpu(core_tm));
                }
        }

        if (thread_get_status(target) != thread_status_zombie) {
                pr_error(
                        "[clean_server] THREAD_REAP: not zombie (status=%lu)\n",
                        thread_get_status(target));
                return;
        }

#ifdef LINUX_COMPAT_TEST
        linux_thread_append_t *ta = linux_thread_append(target);
        if (ta && ta->test_cookie != 0 && target->tm) {
                linux_user_test_notify_exit(
                        (i32)target->tm->owner_cpu, ta->test_cookie, exit_code);
        }
#endif

        error_t e = delete_thread(target);
        if (e != REND_SUCCESS) {
                pr_error("[clean_server] THREAD_REAP: delete_thread failed e=%d\n",
                         (int)e);
        } else if (task) {
                linux_proc_append_t *pa = linux_proc_append(task);
                u32 threads_left;

                lock_cas(&task->thread_list_lock);
                threads_left = (u32)task->thread_number;
                unlock_cas(&task->thread_list_lock);

                /*
                 * Approach 2: notify when task shell is empty and child is a
                 * waitable zombie. Route to parent wait_port or kernel_port.
                 */
                if (threads_left == 0 && pa && pa->exit_state == 1
                    && task_pid > 0) {
                        Tcb_Base *parent = NULL;

                        if (pa->ppid > 0) {
                                parent = find_task_by_pid(pa->ppid);
                        }

                        if (parent) {
                                (void)linux_proc_post_exit_notify(
                                        pa->ppid,
                                        task_pid,
                                        pa->exit_code);
                        } else {
                                (void)linux_proc_post_kernel_exit_notify(
                                        task_pid,
                                        pa->exit_code);
                        }
                }
        }
}

/*
 * TASK_REAP: core delete_task() only. Lookup by pid (idempotent).
 * Requires thread_number==0 and exit_state==2 (wait4 or orphan exit).
 */
static void clean_handle_task_reap(const kmsg_t *km)
{
        i32 pid;
        Tcb_Base *task;
        linux_proc_append_t *pa;
        bool task_empty;
        bool reaped;

        if (ipc_serial_decode(km->payload,
                              km->hdr.payload_len,
                              LINUX_KMSG_FMT_TASK_REAP,
                              &pid)
            != REND_SUCCESS) {
                pr_error("[clean_server] TASK_REAP: decode failed\n");
                return;
        }

        if (pid <= 0) {
                pr_error("[clean_server] TASK_REAP: invalid pid=%d\n", (int)pid);
                return;
        }

        for (;;) {
                task = find_task_by_pid((pid_t)pid);
                if (!task) {
                        return;
                }

                pa = linux_proc_append(task);
                lock_cas(&task->thread_list_lock);
                task_empty = (task->thread_number == 0);
                unlock_cas(&task->thread_list_lock);

#ifdef LINUX_COMPAT_TEST
                reaped = pa && pa->exit_state == 2;
#else
                reaped = true;
#endif
                if (task_empty && reaped) {
                        break;
                }
                if (!task_empty) {
                        schedule(percpu(core_tm));
                        continue;
                }
                pr_error(
                        "[clean_server] TASK_REAP: pid=%d not reaped (exit_state=%d)\n",
                        (int)pid,
                        pa ? (int)pa->exit_state : -1);
                return;
        }

        if (task->vs == &root_vspace) {
                pr_error(
                        "[ Error ] TASK_REAP: user task must not use root vspace\n");
        }

        error_t e = delete_task(task);
        if (e != REND_SUCCESS) {
                pr_error("[clean_server] TASK_REAP: delete_task failed (task=%p, e=%d)\n",
                         (void *)task,
                         (int)e);
        }
}

static void clean_handle_message(Message_t *msg)
{
        const kmsg_t *km;

        if (!msg || !msg->data) {
                pr_error("[clean_server] NULL msg or msg->data\n");
                return;
        }

        km = kmsg_from_msg(msg);
        if (!km) {
                pr_error("[clean_server] NULL kmsg\n");
                return;
        }

        if (!km || km->hdr.module != clean_server_service_id) {
                pr_error("[clean_server] Invalid kmsg module\n");
                return;
        }

        switch (km->hdr.opcode) {
        case KMSG_OP_CLEAN_THREAD_REAP:
                clean_handle_thread_reap(km);
                return;
        case KMSG_OP_CLEAN_TASK_REAP:
                clean_handle_task_reap(km);
                return;
        default:
                pr_error("[clean_server] Unknown opcode %u\n",
                         (unsigned)km->hdr.opcode);
        }
}

static void clean_server_on_message(Message_t *msg, u16 service_id)
{
        (void)service_id;
        clean_handle_message(msg);
}

void clean_server_thread(void)
{
        ipc_server_recv_loop(CLEAN_SERVER_PORT_NAME, clean_server_on_message);
}

static void clean_server_init(void)
{
        if (linux_init_bsp_once(&clean_server_port_registered)) {
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

                error_t err = register_port(global_port_table, port);
                if (err) {
                        pr_error("[clean_server] failed to register port: %d\n",
                                 (int)err);
                        delete_message_port_structure(port);
                        return;
                }
                linux_init_bsp_mark_done(&clean_server_port_registered);
        }

        if (clean_server_service_id == 0) {
                Message_Port_t *p = thread_lookup_port(CLEAN_SERVER_PORT_NAME);
                if (p) {
                        clean_server_service_id = p->service_id;
                        ref_put(&p->refcount, free_message_port_ref);
                } else {
                        pr_error(
                                "[clean_server] CPU %lu: Failed to find clean_server_port\n",
                                (u64)percpu(cpu_number));
                }
        }

        error_t e = gen_thread_from_func(&percpu(clean_server_thread_ptr),
                                         (kthread_func)clean_server_thread,
                                         clean_server_thread_name,
                                         percpu(core_tm),
                                         NULL);
        if (e != REND_SUCCESS) {
                pr_error("[ Error ]clean server init fail (e=%d)\n", (int)e);
        }
}
DEFINE_INIT(clean_server_init);
