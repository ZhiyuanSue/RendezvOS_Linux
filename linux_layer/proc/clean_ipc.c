#include <linux_compat/ipc/clean_protocol.h>
#include <linux_compat/proc/clean_ipc.h>

#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/task/tcb.h>

static error_t linux_clean_deliver_message(Message_t* msg, Message_Port_t* port)
{
        error_t e;

        e = enqueue_msg_for_send(msg);
        if (e != REND_SUCCESS) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                ref_put(&port->refcount, free_message_port_ref);
                pr_error("[clean_ipc] enqueue_msg_for_send failed e=%d\n", (int)e);
                return e;
        }

        e = send_msg(port);
        if (e != REND_SUCCESS) {
                pr_error("[clean_ipc] send_msg failed e=%d\n", (int)e);
        }
        ref_put(&port->refcount, free_message_port_ref);
        return e;
}

error_t linux_clean_send_thread_reap(Thread_Base* thread, i64 exit_code)
{
        Message_Port_t* port;
        Msg_Data_t* md;
        Message_t* msg;

        if (!thread) {
                return -E_IN_PARAM;
        }

        port = thread_lookup_port(CLEAN_SERVER_PORT_NAME);
        if (!port) {
                pr_error("[clean_ipc] port %s not found\n", CLEAN_SERVER_PORT_NAME);
                return -E_RENDEZVOS;
        }

        md = kmsg_create(port->service_id,
                         KMSG_OP_CLEAN_THREAD_REAP,
                         LINUX_KMSG_FMT_THREAD_REAP,
                         thread,
                         exit_code);
        if (!md) {
                ref_put(&port->refcount, free_message_port_ref);
                pr_error("[clean_ipc] THREAD_REAP kmsg_create failed\n");
                return -E_RENDEZVOS;
        }

        msg = create_message_with_msg(md);
        ref_put(&md->refcount, md->free_data);
        if (!msg) {
                ref_put(&port->refcount, free_message_port_ref);
                return -E_RENDEZVOS;
        }

        return linux_clean_deliver_message(msg, port);
}

error_t linux_clean_send_task_reap(pid_t pid)
{
        Message_Port_t* port;
        Msg_Data_t* md;
        Message_t* msg;

        if (pid <= 0) {
                return -E_IN_PARAM;
        }

        port = thread_lookup_port(CLEAN_SERVER_PORT_NAME);
        if (!port) {
                pr_error("[clean_ipc] port %s not found\n", CLEAN_SERVER_PORT_NAME);
                return -E_RENDEZVOS;
        }

        md = kmsg_create(port->service_id,
                         KMSG_OP_CLEAN_TASK_REAP,
                         LINUX_KMSG_FMT_TASK_REAP,
                         (i32)pid);
        if (!md) {
                ref_put(&port->refcount, free_message_port_ref);
                pr_error("[clean_ipc] TASK_REAP kmsg_create failed\n");
                return -E_RENDEZVOS;
        }

        msg = create_message_with_msg(md);
        ref_put(&md->refcount, md->free_data);
        if (!msg) {
                ref_put(&port->refcount, free_message_port_ref);
                return -E_RENDEZVOS;
        }

        return linux_clean_deliver_message(msg, port);
}
