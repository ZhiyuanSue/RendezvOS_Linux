/*
 * Wake threads blocked in recv_msg(port) for signal EINTR (RPC reply ports, …).
 */

#include <linux_compat/ipc/block_wake.h>
#include <linux_compat/signal/signal_deliver.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/kmsg_system.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/task/tcb.h>

bool linux_ipc_kmsg_is_port_closed(Message_Port_t *port, const Message_t *msg)
{
        const kmsg_t *km;

        if (!port || !msg) {
                return false;
        }

        km = kmsg_from_msg(msg);
        if (!km || km->hdr.module != port->service_id) {
                return false;
        }
        return km->hdr.opcode == KMSG_OP_SYSTEM_PORT_CLOSED;
}

bool linux_ipc_post_recv_interrupt(Message_Port_t *port)
{
        Msg_Data_t *md;
        Message_t *msg;
        error_t err;

        if (!port) {
                return false;
        }

        md = kmsg_create(port->service_id,
                         KMSG_OP_IPC_RECV_INTERRUPT,
                         LINUX_KMSG_FMT_IPC_RECV_INTERRUPT,
                         (i64)0);
        if (!md) {
                return false;
        }

        msg = create_message_with_msg(md);
        ref_put(&md->refcount, free_msgdata_ref_default);
        if (!msg) {
                return false;
        }

        err = ipc_system_try_deliver(port, msg, false);
        return err == REND_SUCCESS;
}

void linux_ipc_recv_wake_for_signal(Thread_Base *thread, Message_Port_t *port)
{
        if (!thread || !port) {
                return;
        }
        if (thread_get_status(thread) != thread_status_block_on_receive) {
                return;
        }
        if ((Message_Port_t *)thread->port_ptr != port) {
                return;
        }
        if (!linux_signal_thread_has_deliverable_pending(thread)) {
                return;
        }

        (void)linux_ipc_post_recv_interrupt(port);
}
