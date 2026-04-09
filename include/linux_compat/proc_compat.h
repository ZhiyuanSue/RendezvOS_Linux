#ifndef _RENDEZVOS_LINUX_COMPAT_PROC_COMPAT_H_
#define _RENDEZVOS_LINUX_COMPAT_PROC_COMPAT_H_

#include <common/types.h>
#include <rendezvos/task/tcb.h>

/*
 * Linux compat append model:
 * - linux-layer state lives in the append area of Tcb_Base / Thread_Base
 * - core does not interpret these bytes
 *
 * IMPORTANT: the creator path must pass append sizes consistently (single
 * source of truth: these macros).
 */

typedef struct linux_proc_append {
        u64 start_brk;
        u64 brk;
} linux_proc_append_t;

typedef struct linux_thread_append {
        u64 clear_tid; /* user pointer for set_tid_address/CLONE_CHILD_CLEARTID */
        /*
         * Test runner cookie (TEST only): set by the linux compat test runner
         * to correlate thread exit with a waiting kernel runner thread.
         *
         * Default 0 means "not a test-managed user thread".
         */
        u64 test_cookie;
} linux_thread_append_t;

#define LINUX_PROC_APPEND_BYTES   ((size_t)sizeof(linux_proc_append_t))
#define LINUX_THREAD_APPEND_BYTES ((size_t)sizeof(linux_thread_append_t))

static inline linux_proc_append_t* linux_proc_append(Tcb_Base* tcb)
{
        if (!tcb)
                return NULL;
        return (linux_proc_append_t*)tcb->append_tcb_info;
}

static inline linux_thread_append_t* linux_thread_append(Thread_Base* thread)
{
        if (!thread)
                return NULL;
        return (linux_thread_append_t*)thread->append_thread_info;
}

#endif

