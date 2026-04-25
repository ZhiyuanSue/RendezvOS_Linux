#ifndef _RENDEZVOS_LINUX_COMPAT_PROC_REGISTRY_H_
#define _RENDEZVOS_LINUX_COMPAT_PROC_REGISTRY_H_

#include <common/types.h>
#include <rendezvos/error.h>
#include <linux_compat/proc_compat.h>

/*
 * Process registry for PID lookup.
 *
 * Provides O(1) PID->task lookup using core's name_index mechanism.
 * Also supports reverse lookups for wait4 extensions (by ppid/pgid).
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
 * Find a child task by parent PID.
 * Returns the first zombie child, or NULL if no zombie children found.
 *
 * @param ppid: Parent PID to search for
 * @return: Zombie child task if found, NULL otherwise
 */
Tcb_Base* find_zombie_child(pid_t ppid);

/*
 * Find a zombie child in the same process group.
 * Returns the first zombie child with matching pgid, or NULL if none found.
 *
 * @param ppid: Parent PID (to verify it's actually our child)
 * @param pgid: Process group ID to match
 * @return: Zombie child task if found, NULL otherwise
 */
Tcb_Base* find_zombie_child_in_pgid(pid_t ppid, pid_t pgid);

/*
 * Unregister a process from the registry.
 *
 * @param task: Task to unregister
 */
void unregister_process(Tcb_Base* task);

#endif
