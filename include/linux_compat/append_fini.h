#ifndef _RENDEZVOS_LINUX_COMPAT_APPEND_FINI_H_
#define _RENDEZVOS_LINUX_COMPAT_APPEND_FINI_H_

#include <common/types.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>

/*
 * Linux compat append lifecycle hooks (mirrors core append_fini / append_copy).
 * Pointers are wired in linux_elf_init; core invokes thread hook from
 * copy_thread; task hook is invoked by linux_layer on the dst TCB.
 */
extern task_append_fini_t linux_task_append_fini_ptr;
extern task_append_copy_t linux_task_append_fork_ptr;
extern thread_append_fini_t linux_thread_append_fini_ptr;
extern thread_append_copy_t linux_thread_append_fork_ptr;

void linux_task_append_fini(struct Tcb_Base *tcb);
error_t linux_task_append_fork(struct Tcb_Base *child, struct Tcb_Base *parent);
void linux_thread_append_fini(struct Thread_Base *thread);
error_t linux_thread_append_fork(struct Thread_Base *child,
                                 struct Thread_Base *parent);

#endif
