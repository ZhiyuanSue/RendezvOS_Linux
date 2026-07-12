#ifndef _RENDEZVOS_LINUX_COMPAT_APPEND_HOOKS_H_
#define _RENDEZVOS_LINUX_COMPAT_APPEND_HOOKS_H_

#include <common/types.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/thread_loader.h>

/*
 * Linux compat append lifecycle hooks (mirrors core task/thread_append_hooks).
 * Static tables are wired in linux_elf_init.c.
 */
extern const task_append_hooks_t linux_task_append_hooks;
extern const thread_append_hooks_t linux_thread_append_hooks;

void linux_task_append_fini(struct Tcb_Base *tcb);
error_t linux_task_append_fork(struct Tcb_Base *child, struct Tcb_Base *parent);
void linux_thread_append_fini(struct Thread_Base *thread);
error_t linux_thread_append_fork(struct Thread_Base *child,
                                 struct Thread_Base *parent);
error_t linux_thread_append_init(struct Thread_Base *thread,
                                 const elf_load_info_t *info);

#endif
