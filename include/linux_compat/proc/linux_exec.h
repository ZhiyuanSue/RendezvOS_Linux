#ifndef _LINUX_COMPAT_PROC_LINUX_EXEC_H_
#define _LINUX_COMPAT_PROC_LINUX_EXEC_H_

#include <common/stdbool.h>
#include <common/types.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>

#define LINUX_EXEC_MAX_PATH 256
#define LINUX_EXEC_MAX_ARGS 128
#define LINUX_EXEC_MAX_ARG_LEN 256

/*
 * Shared image replace for sys_execve and kernel PID1 boot.
 *
 * Loads @filename from initramfs/VFS into @task->vs, builds the standard
 * Linux user stack (argv + auxv via linux_exec_build_initial_stack), resets
 * proc/thread exec state. Does not touch trap_frame / return-to-user.
 *
 * @may_abort_after_clear: if true (syscall path), failures after
 * vspace_clear_user_mappings terminate via linux_fatal_user_fault; if false
 * (PID1 boot on a fresh vs), return a negative Linux errno instead.
 *
 * On success: *entry_out / *sp_out set; returns 0.
 */
i64 linux_exec_replace_image(Tcb_Base *task, Thread_Base *thread,
                             const char *filename, i64 argc,
                             const char *const kargv[],
                             bool may_abort_after_clear, vaddr *entry_out,
                             vaddr *sp_out);

/*
 * Attach Linux proc/thread state for a brand-new user task (PID1).
 * Does not load an ELF or build a user stack.
 */
error_t linux_user_task_prepare_new(Tcb_Base *task, Thread_Base *thread);

#endif /* _LINUX_COMPAT_PROC_LINUX_EXEC_H_ */
