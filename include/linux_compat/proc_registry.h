#ifndef _RENDEZVOS_LINUX_COMPAT_PROC_REGISTRY_H_
#define _RENDEZVOS_LINUX_COMPAT_PROC_REGISTRY_H_

#include <common/types.h>
#include <rendezvos/error.h>
#include <linux_compat/proc_compat.h>
#include <rendezvos/ipc/port.h>

#define PROC_PID_STR_MAX        16
#define PROC_WAIT_PORT_NAME_MAX 32

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

/** Decimal PID string; returns bytes written (excluding NUL), or 0 on
 * truncation. */
size_t proc_format_pid(char* buf, size_t bufsize, pid_t pid);

/** Format @c wait_port_<pid> ; returns bytes written, or 0 on truncation. */
size_t proc_format_wait_port_name(char* buf, size_t bufsize, pid_t pid);

/** Lookup or create+register the per-process wait IPC port. */
Message_Port_t* proc_get_or_create_wait_port(pid_t pid);

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
 * True if @p ppid has a non-reaped child (running or zombie).
 * When @p filter_by_pgid, only children in @p pgid match.
 */
bool proc_parent_has_unreaped_child(pid_t ppid, pid_t pgid,
                                    bool filter_by_pgid);

/*
 * Unregister a process from the registry.
 *
 * @param task: Task to unregister
 */
void unregister_process(Tcb_Base* task);

#endif
