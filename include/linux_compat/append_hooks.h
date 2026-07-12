#ifndef _RENDEZVOS_LINUX_COMPAT_APPEND_HOOKS_H_
#define _RENDEZVOS_LINUX_COMPAT_APPEND_HOOKS_H_

#include <common/types.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/thread_loader.h>

/*
 * Linux compat append lifecycle hooks — mirrors core task/thread_append_hooks.
 *
 * Wire-up: linux_layer/loader/linux_elf_init.c defines the static tables.
 * Policy:  doc/linux_compat/APPEND_HOOKS.md
 *
 * Callers pass &linux_task_append_hooks / &linux_thread_append_hooks to
 * new_task_structure, create_thread, gen_task_from_elf; core invokes
 * copy/fini (and thread init from run_elf_program).
 */
extern const task_append_hooks_t linux_task_append_hooks;
extern const thread_append_hooks_t linux_thread_append_hooks;

/* task append: fini on delete_task; copy after fork/clone proc setup */
void linux_task_append_fini(Tcb_Base *tcb);
error_t linux_task_append_copy(Tcb_Base *dst, Tcb_Base *src);

/* clone(2): CLONE_VM uses signal attach + shared fs; else same as fork copy */
error_t linux_task_append_clone(Tcb_Base *dst, Tcb_Base *src, u64 clone_flags);

/* thread append: init on first ELF exec; copy from copy_thread; fini on thread delete */
void linux_thread_append_fini(Thread_Base *thread);
error_t linux_thread_append_copy(Thread_Base *dst, Thread_Base *src);
error_t linux_thread_append_init(Thread_Base *thread,
                                 const elf_load_info_t *info);

#endif
