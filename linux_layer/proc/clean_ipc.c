#include <linux_compat/ipc/clean_protocol.h>
#include <linux_compat/ipc/port_naming.h>
#include <linux_compat/proc/clean_ipc.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/thread.h>

extern struct Port_Table* global_port_table;

static void linux_clean_log_lookup_miss(const char* what)
{
        Thread_Base* self = get_cpu_current_thread();
        Task_Manager* tm = percpu(core_tm);
        Message_Port_t* via_table = NULL;

        if (global_port_table) {
                via_table = port_table_lookup(global_port_table,
                                              CLEAN_SERVER_PORT_NAME);
        }

        pr_error("[clean_ipc] %s: '%s' not found via thread_lookup_port "
                 "(self=%p tm=%p gpt=%p via_table=%p cpu=%lu)\n",
                 what,
                 CLEAN_SERVER_PORT_NAME,
                 (void*)self,
                 (void*)tm,
                 (void*)global_port_table,
                 (void*)via_table,
                 (u64)percpu(cpu_number));

        if (via_table)
                ref_put(&via_table->refcount, free_message_port_ref);
}

static error_t linux_clean_deliver_message(Message_t* msg, Message_Port_t* port)
{
        error_t e;

        e = enqueue_msg_for_send(msg);
        if (e != REND_SUCCESS) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                ref_put(&port->refcount, free_message_port_ref);
                pr_error("[clean_ipc] enqueue_msg_for_send failed e=%d\n",
                         (int)e);
                return e;
        }

        e = send_msg(port);
        ref_put(&port->refcount, free_message_port_ref);
        if (e == -E_REND_PORT_CLOSED)
                return REND_SUCCESS;
        if (e != REND_SUCCESS) {
                pr_error("[clean_ipc] send_msg failed e=%d\n", (int)e);
        }
        return e;
}

error_t linux_clean_send_thread_reap(Thread_Base* thread, i64 exit_code)
{
        Message_Port_t* port;
        Msg_Data_t* md;
        Message_t* msg;

        if (!thread)
                return -E_IN_PARAM;

        port = thread_lookup_port(CLEAN_SERVER_PORT_NAME);
        if (!port) {
                linux_clean_log_lookup_miss("THREAD_REAP");
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
