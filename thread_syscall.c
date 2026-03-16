
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/ipc.h>
#include <modules/log/log.h>
extern Message_Port_t* clean_server_port;
void sys_exit(i64 exit_code)
{
        Thread_Base* self = get_cpu_current_thread();
        if (self && percpu(clean_server_port)) {
                struct allocator* a = percpu(kallocator);
                typedef struct {
                        Thread_Base* thread;
                        i64 exit_code;
                } thread_exit_req_t;
                thread_exit_req_t* req = (thread_exit_req_t*)a->m_alloc(
                        a, sizeof(thread_exit_req_t));
                if (req) {
                        req->thread = self;
                        req->exit_code = exit_code;
                        void* data = (void*)req;
                        Msg_Data_t* md =
                                create_message_data(1,
                                                    sizeof(thread_exit_req_t),
                                                    &data,
                                                    free_msgdata_ref_default);
                        if (md) {
                                Message_t* msg = create_message_with_msg(md);
                                ref_put(&md->refcount, md->free_data);
                                if (msg) {
                                        if (enqueue_msg_for_send(msg)
                                            == REND_SUCCESS) {
                                                (void)send_msg(percpu(clean_server_port));
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
        } else {
                pr_error("[ Syscall ] thread %s not found clean_server_port",
                         self->name);
        }

        if (self) {
                thread_set_status(self, thread_status_zombie);
        }
        schedule(percpu(core_tm));
        while (1) {
                schedule(percpu(core_tm));
        }
}