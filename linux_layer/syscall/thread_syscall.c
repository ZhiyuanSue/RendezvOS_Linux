#include <modules/log/log.h>
#include <common/types.h>
#include <rendezvos/error.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>

#define CLEAN_SERVER_PORT_NAME     "clean_server_port"
#define CLEAN_KMSG_FMT_THREAD_REAP "p q"

void sys_exit(i64 exit_code)
{
        Thread_Base* self = get_cpu_current_thread();

        if (!self)
                goto out;

        /*
         * Exit intent: THREAD_FLAG_EXIT_REQUESTED (survives IPC status
         * changes). Owner CPU scheduler moves running -> zombie on switch-away.
         */
        thread_or_flags(self, THREAD_FLAG_EXIT_REQUESTED);

        pr_debug(
                "[sys_exit] CPU %lu: thread %s (tid=%lu) requesting exit with code %ld\n",
                (u64)percpu(cpu_number),
                self->name ? self->name : "(unnamed)",
                (u64)self->tid,
                exit_code);

        Message_Port_t* port = thread_lookup_port(CLEAN_SERVER_PORT_NAME);
        if (!port) {
                pr_error("[sys_exit] port %s not found\n",
                         CLEAN_SERVER_PORT_NAME);
                goto out;
        }

        Msg_Data_t* md = kmsg_create(port->service_id,
                                     KMSG_OP_CORE_THREAD_REAP,
                                     CLEAN_KMSG_FMT_THREAD_REAP,
                                     self,
                                     exit_code);
        if (!md) {
                ref_put(&port->refcount, free_message_port_ref);
                pr_error("[sys_exit] kmsg_create failed\n");
                goto out;
        }

        Message_t* msg = create_message_with_msg(md);
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

        (void)send_msg(port);
        ref_put(&port->refcount, free_message_port_ref);

out:
        schedule(percpu(core_tm));
        while (1) {
                schedule(percpu(core_tm));
        }
}

void sys_exit_group(i64 exit_code)
{
        /* Single-thread world for now: exit_group == exit. */
        sys_exit(exit_code);
}
