#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <modules/log/log.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

/*
 * set_tid_address syscall implementation for Linux compatibility.
 *
 * This syscall sets the address where the child thread ID will be cleared
 * when the thread exits. This is used by pthread libraries for join notification.
 *
 * When the thread exits, the kernel will:
 * 1. Clear the tidptr to 0
 * 2. Wake up any futex waiters on that address
 *
 * This allows pthread_join to work efficiently.
 *
 * Signature: int set_tid_address(int *tidptr);
 * Returns: current thread ID (gettid())
 */

i64 sys_set_tid_address(u64 tidptr)
{
        Thread_Base *current_thread = get_cpu_current_thread();
        linux_thread_append_t *append;

        if (!current_thread) {
                pr_error("[SET_TID_ADDRESS] Invalid current thread\n");
                return -LINUX_ESRCH;
        }

        append = linux_thread_append(current_thread);
        if (!append) {
                pr_error("[SET_TID_ADDRESS] No thread append area\n");
                return -LINUX_ESRCH;
        }

        /* Store the tidptr for clearing on exit */
        append->clear_tid = tidptr;

        pr_debug("[SET_TID_ADDRESS] Thread %d set clear_tid to 0x%llx\n",
                 current_thread->tid,
                 tidptr);

        /* Return current thread ID */
        return (i64)current_thread->tid;
}
