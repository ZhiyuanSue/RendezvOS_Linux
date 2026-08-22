#ifndef _RENDEZVOS_LINUX_COMPAT_APPEND_HOOKS_H_
#define _RENDEZVOS_LINUX_COMPAT_APPEND_HOOKS_H_

#include <common/types.h>
#include <rendezvos/error.h>
#include <rendezvos/task/thread.h>
#include <rendezvos/task/thread_loader.h>

/*
 * Linux thread append lifecycle — core invokes init/copy/fini.
 *
 * Process state is heap linux_proc_resource_t in thread append `res`
 * (linux_proc_alloc / linux_proc_attach_thread). No task append on core.
 *
 * Wire-up: linux_layer/loader/linux_elf_init.c
 * Policy:  doc/linux_compat/APPEND_HOOKS.md
 */
extern const thread_append_hooks_t linux_thread_append_hooks;

void linux_thread_append_fini(Thread_Base *thread);
error_t linux_thread_append_copy(Thread_Base *dst, Thread_Base *src);

#endif
