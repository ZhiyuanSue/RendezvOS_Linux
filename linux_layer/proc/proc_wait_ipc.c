/*
 * wait4 port wake: child exit notify + signal EINTR interrupt (see sys_wait.c).
 */

#include <linux_compat/ipc/exit_protocol.h>
#include <linux_compat/proc/wait_ipc.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/signal/signal_deliver.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/task/tcb.h>

static bool linux_proc_wait_post_interrupt(Message_Port_t *port)
{
        Msg_Data_t *md;
        Message_t *msg;
        error_t err;

        if (!port) {
                return false;
        }

        md = kmsg_create(port->service_id,
                         KMSG_OP_PROC_WAIT_INTERRUPT,
                         LINUX_KMSG_FMT_WAIT_INTERRUPT,
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

void linux_proc_wait_wake_for_signal(Thread_Base *thread, Tcb_Base *process)
{
        Message_Port_t *wait_port;

        if (!thread || !process) {
                return;
        }
        if (thread_get_status(thread) != thread_status_block_on_receive) {
                return;
        }
        if (!linux_signal_wait4_should_return_eintr(thread)) {
                return;
        }

        wait_port = proc_get_or_create_wait_port(process->pid);
        if (!wait_port) {
                return;
        }
        if ((Message_Port_t *)thread->port_ptr != wait_port) {
                return;
        }

        (void)linux_proc_wait_post_interrupt(wait_port);
}
