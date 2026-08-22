#ifndef _RENDEZVOS_LINUX_COMPAT_PROC_REGISTRY_H_
#define _RENDEZVOS_LINUX_COMPAT_PROC_REGISTRY_H_

#include <common/types.h>
#include <rendezvos/error.h>
#include <linux_compat/proc_compat.h>
#include <rendezvos/ipc/port.h>

#define PROC_PID_STR_MAX        16
#define PROC_WAIT_PORT_NAME_MAX 32
/** Linear scan cap for reparent / live-child lookup (TODO: reverse index). */
#define PROC_PID_SCAN_MAX 4096

/*
 * Process registry: O(1) pid → linux_proc_resource_t via name_index.
 */

void proc_registry_init(void);

size_t proc_format_pid(char* buf, size_t bufsize, pid_t pid);

size_t proc_format_wait_port_name(char* buf, size_t bufsize, pid_t pid);

Message_Port_t* proc_get_or_create_wait_port(pid_t pid);

void proc_unregister_wait_port(pid_t pid);

error_t register_process(linux_proc_resource_t* proc);

linux_proc_resource_t* find_proc_by_pid(pid_t pid);

/*
 * True if @p ppid has a non-reaped child (RUNNING or ZOMBIE).
 * When @p filter_by_pgid, only children in @p pgid match.
 */
bool proc_parent_has_unreaped_child(pid_t ppid, pid_t pgid,
                                    bool filter_by_pgid);

void unregister_process(linux_proc_resource_t* proc);

/** Reparent live children of @p old_ppid to @p new_ppid (on parent reap). */
void proc_reparent_children(pid_t old_ppid, pid_t new_ppid);

/*
 * Link A: live parent can wait4 on wait_port_<ppid>.
 * ppid==0 or dead parent → Link B (clean inline reap). See EXIT_CLEAN.md.
 */
bool proc_has_wait_reaper(linux_proc_resource_t* proc);

#endif
