#include <modules/log/log.h>
#include <common/types.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/ipc.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/port.h>

#define CLEAN_SERVER_PORT_NAME "clean_server_port"

void sys_exit(i64 exit_code)
{
        Thread_Base *self = get_cpu_current_thread();
        Message_Port_t *clean_port = NULL;
        
        if (!self)
                goto out;

        /*
         * Exit intent: THREAD_FLAG_EXIT_REQUESTED (survives IPC status changes).
         * Owner CPU scheduler moves running -> zombie on switch-away.
         */
        thread_or_flags(self, THREAD_FLAG_EXIT_REQUESTED);

        /* Look up global clean server port via global port table */
        clean_port = thread_lookup_port(CLEAN_SERVER_PORT_NAME);
        if (!clean_port) {
                pr_error("[ Syscall ] thread %s: %s not found in global table",
                         self->name,
                         CLEAN_SERVER_PORT_NAME);
                goto out;
        }
        
        if (clean_port) {
                struct allocator *a = percpu(kallocator);
                typedef struct {
                        Thread_Base *thread;
                        i64 exit_code;
                } thread_exit_req_t;
                thread_exit_req_t *req = (thread_exit_req_t *)a->m_alloc(
                        a, sizeof(thread_exit_req_t));
                if (req) {
                        req->thread = self;
                        req->exit_code = exit_code;
                        void *data = (void *)req;
                        Msg_Data_t *md =
                                create_message_data(1,
                                                    sizeof(thread_exit_req_t),
                                                    &data,
                                                    free_msgdata_ref_default);
                        if (md) {
                                Message_t *msg = create_message_with_msg(md);
                                ref_put(&md->refcount, md->free_data);
                                if (msg) {
                                        if (enqueue_msg_for_send(msg)
                                            == REND_SUCCESS) {
                                                (void)send_msg(clean_port);
                                        } else {
                                                ref_put(&msg->ms_queue_node
                                                                 .refcount,
                                                        free_message_ref);
                                        }
                                }
                        } else {
                                a->m_free(a, req);
                        }
                }
                /* Release the reference acquired by thread_lookup_port */
                ref_put(&clean_port->refcount, free_message_port_ref);
        }
out:
        schedule(percpu(core_tm));
        while (1) {
                schedule(percpu(core_tm));
        }
        
        /* Note: thread_lookup_port returns a reference that we hold.
         * We release the reference after sending the message (or if sending fails).
         * The port remains valid because clean server threads hold their own references.
         */
}
