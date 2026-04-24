#ifndef _RENDEZVOS_LINUX_COMPAT_PROC_REGISTRY_H_
#define _RENDEZVOS_LINUX_COMPAT_PROC_REGISTRY_H_

#include <common/types.h>
#include <rendezvos/error.h>
#include <linux_compat/proc_compat.h>

/*
 * Process registry for PID lookup.
 *
 * Provides O(1) PID->task lookup using core's name_index mechanism.
 */

/*
 * Initialize the process registry.
 * Must be called before any other registry functions.
 */
void proc_registry_init(void);

/*
 * Register a process in the registry.
 *
 * @param task: Task to register
 * @return: 0 on success, negative errno on failure
 */
error_t register_process(Tcb_Base* task);

/*
 * Find a task by PID.
 *
 * @param pid: Process ID to search for
 * @return: Task if found, NULL otherwise
 */
Tcb_Base* find_task_by_pid(pid_t pid);

/*
 * Unregister a process from the registry.
 *
 * @param task: Task to unregister
 */
void unregister_process(Tcb_Base* task);

#endif
