#include <modules/log/log.h>
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

DEFINE_PER_CPU(Message_Port_t *, clean_server_port);
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
        if (thread_get_status(target) != thread_status_zombie)
                return;

        Tcb_Base *task = target->belong_tcb;
        delete_thread(target);
        if (task && task->thread_number == 0) {
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
                delete_task(task);
        }
}

void clean_server_thread(void)
{
        if (!percpu(clean_server_port)) {
                percpu(clean_server_port) =
                        create_message_port(clean_server_port_name);
        }

        while (1) {
                Message_Port_t *port = percpu(clean_server_port);
                if (!port) {
                        schedule(percpu(core_tm));
                        continue;
                }

                (void)recv_msg(port);

                while (1) {
                        Message_t *msg = dequeue_recv_msg();
                        if (!msg)
                                break;
                        clean_handle_message(msg);
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                }
        }
}

extern u32 BSP_ID;

static void clean_server_init(void)
{
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