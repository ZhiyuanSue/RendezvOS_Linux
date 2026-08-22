#ifndef _RENDEZVOS_LINUX_COMPAT_PROC_COMPAT_H_
#define _RENDEZVOS_LINUX_COMPAT_PROC_COMPAT_H_

#include <common/dsa/list.h>
#include <common/refcount.h>
#include <common/stddef.h>
#include <common/types.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/signal/signal_state.h>
#include <rendezvos/error.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/sync/cas_lock.h>
#include <rendezvos/task/thread.h>
#include <rendezvos/time.h>

/*
 * Linux shared process resource bundle (heap). Threads hold `ta->res`;
 * core has no process object.
 */

typedef struct linux_proc_resource {
        ref_count_t refcount;
        pid_t pid;
        cas_lock_t thread_list_lock;
        i64 thread_number;
        struct list_entry thread_head_node;

        /* Memory management */
        u64 start_brk;
        u64 brk;
        u64 mmap_hint; /* Anonymous mmap search cursor (page-aligned VA past
                          last mmap) */

        /* Process relationships */
        u32 uid;
        u32 gid;
        u32 euid;
        u32 egid;
        /*
         * Parent PID. LINUX_INIT_REAP_PPID (0) = init-adopted / orphan →
         * Link B (clean inline linux_proc_reap). Live parent → Link A.
         */
        pid_t ppid;
        pid_t pgid; /* Process group ID (for wait4 pid==0, pid<-1) */
        i32 exit_code; /* Exit code for wait() */
        /*
         * Exit / clean protocol (see doc/linux_compat/protocols/EXIT_CLEAN.md):
         *   0 RUNNING, 1 ZOMBIE, 2 NOTIFIED, 3 CLAIMED
         */
        i32 exit_state;
        /*
         * Hint at sys_exit: this thread was the only member (thread_number==1).
         * Clean sets this authoritatively when thread_number reaches 0 after
         * detach (covers concurrent last-two-thread exit). wait4 checks this.
         */
        u8 exit_last_thread;
        /* EXIT_NOTIFY not yet consumed by wait4 (wait(pid) mismatch). */
        struct list_entry pending_exits;

        /*
         * Heap-backed Linux state (append = pointer only).
         */
        linux_signal_proc_state_t *signal;
        linux_fs_state_t *fs;
        /* Non-owning cache of the process address space (threads hold refs). */
        VSpace *vs;
} linux_proc_resource_t;

/** ppid after reparent-to-init (Link B orphans; ≠ user /init). */
#define LINUX_INIT_REAP_PPID 0

#define LINUX_EXIT_RUNNING      0
#define LINUX_EXIT_ZOMBIE       1
#define LINUX_EXIT_NOTIFIED     2 /* Link A: EXIT_NOTIFY committed */
#define LINUX_EXIT_CLAIMED      3

typedef struct linux_thread_append {
        /*
         * Heap-backed / IPC fields first — fixed offsets used across
         * linux_layer (signal @ append+0, sleep_port @ +8, …). Do not insert
         * fields before signal without rebuilding all consumers.
         */
        linux_signal_thread_state_t *signal;

        Message_Port_t *sleep_port;
        u64 sleep_timer_token;
        rendezvos_timer_event sleep_timer_event;

        /* Thread management (after stable-prefix fields above). */
        u64 clear_tid; /* user pointer for set_tid_address/CLONE_CHILD_CLEARTID
                        */
        /* Path-B PID1 wait: set only on /init; copy_thread must clear. */
        u64 boot_wait_cookie;

        linux_proc_resource_t *res;
        struct list_entry res_thread_node;
} linux_thread_append_t;

#define LINUX_THREAD_APPEND_BYTES ((size_t)sizeof(linux_thread_append_t))

static inline linux_thread_append_t *linux_thread_append(Thread_Base *thread)
{
        if (!thread)
                return NULL;
        return (linux_thread_append_t *)thread->append_thread_info;
}

static inline linux_proc_resource_t *linux_proc_of(Thread_Base *thread)
{
        linux_thread_append_t *ta = linux_thread_append(thread);

        return ta ? ta->res : NULL;
}

static inline Thread_Base *linux_thread_from_append(linux_thread_append_t *ta)
{
        if (!ta)
                return NULL;
        return (Thread_Base *)((char *)ta
                               - offsetof(Thread_Base, append_thread_info));
}

static inline linux_proc_resource_t *linux_current_proc(void)
{
        return linux_proc_of(get_cpu_current_thread());
}

static inline VSpace *linux_current_vs(void)
{
        Thread_Base *th = get_cpu_current_thread();

        return th ? th->vs : NULL;
}

linux_proc_resource_t *linux_proc_alloc(void);
error_t linux_proc_attach_thread(linux_proc_resource_t *proc, Thread_Base *thread);
void linux_proc_detach_thread(Thread_Base *thread);
void linux_proc_wait_all_threads_detached(linux_proc_resource_t *proc);
void linux_proc_fini(linux_proc_resource_t *proc);
error_t linux_proc_put(linux_proc_resource_t *proc);
error_t linux_proc_reap(linux_proc_resource_t *proc);

error_t linux_proc_copy_from(linux_proc_resource_t *dst, linux_proc_resource_t *src);
error_t linux_proc_clone_from(linux_proc_resource_t *dst, linux_proc_resource_t *src,
                              u64 clone_flags);

#endif
