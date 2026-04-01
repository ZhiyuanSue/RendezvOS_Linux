#include <modules/log/log.h>
#include <common/types.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/task/ipc.h>
#include <rendezvos/task/message.h>
#include <rendezvos/task/port.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/thread_loader.h>

typedef struct {
        Thread_Base *thread;
        i64 exit_code;
} thread_exit_req_t;

/* Per-CPU clean server thread pointers */
DEFINE_PER_CPU(Thread_Base *, clean_server_thread_ptr);

static char clean_server_thread_name[] = "clean_server_thread";
static char clean_server_port_name[] = "clean_server_port";

static void clean_handle_message(Message_t *msg)
{
        if (!msg || !msg->data)
                return;
        if (msg->data->msg_type != 1)
                return;
        thread_exit_req_t *req = (thread_exit_req_t *)msg->data->data;
        if (!req || !req->thread)
                return;

        Thread_Base *target = req->thread;
        Thread_Base *curr = get_cpu_current_thread();
        if (target == curr)
                return;
        if (target == percpu(init_thread_ptr)
            || target == percpu(idle_thread_ptr))
                return;
        /* Wait for owner CPU to mark ZOMBIE (exit intent is THREAD_FLAG_…
         * only). */
        if (target->flags & THREAD_FLAG_EXIT_REQUESTED) {
                while (thread_get_status(target) != thread_status_zombie) {
                        schedule(percpu(core_tm));
                }
        }
        u64 status = thread_get_status(target);
        if (status != thread_status_zombie) {
                pr_error("no zombie\n");
                return;
        }

        /* Debug output: show which thread is being cleaned up on which core */
        pr_debug("[clean_server] CPU %lu: cleaning up thread %s (tid=%lu, exit_code=%ld)\n",
                (u64)percpu(cpu_number),
                target->name ? target->name : "(unnamed)",
                (u64)target->tid,
                req->exit_code);

        Tcb_Base *task = target->belong_tcb;
        delete_thread(target);
        bool task_empty = false;
        if (task) {
                lock_cas(&task->thread_list_lock);
                task_empty = (task->thread_number == 0);
                unlock_cas(&task->thread_list_lock);
        }
        if (task && task_empty) {
                /*we might now in the vspace that need to clean, we must change
                 * the vspace to the root and we must ensure that the task's
                 * vspace is not the root vspace*/
                if (task->vs == &root_vspace) {
                        pr_error(
                                "[ Error ] a user task should not use root vspace as its vspace\n");
                }

                percpu(core_tm)->current_task = percpu(core_tm)->root_task;
                percpu(current_vspace) = percpu(core_tm)->root_task->vs;
                arch_set_current_user_vspace_root(
                        percpu(core_tm)->root_task->vs->vspace_root_addr);
                error_t e = delete_task(task);
                if (e) {
                        pr_error(
                                "[ Error ] delete_task failed (task=%p, e=%d)\n",
                                (void *)task,
                                e);
                }
        }
}

void clean_server_thread(void)
{
        Message_Port_t *port = NULL;

        /* Lookup global clean server port via global table */
        while (!port) {
                port = thread_lookup_port(clean_server_port_name);
                if (!port) {
                        /* Port not yet registered, sleep and retry */
                        schedule(percpu(core_tm));
                        continue;
                }
        }

        /* We hold a reference to the port via thread_lookup_port */
        while (1) {
                error_t ret = recv_msg(port);
                if (ret) {
                        /* recv_msg failed, maybe port deleted?
                         * Release reference and re-lookup port.
                         */
                        ref_put(&port->refcount, free_message_port_ref);
                        port = NULL;
                        while (!port) {
                                port = thread_lookup_port(
                                        clean_server_port_name);
                                if (!port) {
                                        schedule(percpu(core_tm));
                                        continue;
                                }
                        }
                        continue;
                }

                while (1) {
                        Message_t *msg = dequeue_recv_msg();
                        if (!msg)
                                break;
                        clean_handle_message(msg);
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
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

        /* On BSP: create and register global clean server port */
        if (cpu == BSP_ID) {
                if (!global_port_table) {
                        pr_error(
                                "[clean_server] global_port_table not initialized\n");
                        return;
                }
                Message_Port_t *port =
                        create_message_port(clean_server_port_name);
                if (!port) {
                        pr_error(
                                "[clean_server] failed to create message port\n");
                        return;
                }

                error_t err = register_port(global_port_table, port);
                if (err) {
                        pr_error("[clean_server] failed to register port: %d\n",
                                 (int)err);
                        delete_message_port_structure(port);
                        return;
                }
        }

        /* On all CPUs (including BSP): create clean server thread */
        error_t e = gen_thread_from_func(&percpu(clean_server_thread_ptr),
                                         (kthread_func)clean_server_thread,
                                         clean_server_thread_name,
                                         percpu(core_tm),
                                         NULL);
        if (e) {
                pr_error("[ Error ]clean server init fail\n");
        }
}
DEFINE_INIT(clean_server_init);