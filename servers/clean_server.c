#include <modules/log/log.h>
#include <common/types.h>
#include <common/string.h>
#include <common/dsa/list.h>
#include <rendezvos/ipc/ipc_serial.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/thread_loader.h>
#include <rendezvos/sync/cas_lock.h>
#include <linux_compat/errno.h>
#include <linux_compat/ipc/block_wake.h>
#include <linux_compat/ipc/clean_protocol.h>
#include <linux_compat/ipc/rpc.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/proc/wait_ipc.h>
#ifdef LINUX_COMPAT_TEST
#include <linux_compat/boot_wait.h>
#endif

extern struct Port_Table *global_port_table;

DEFINE_PER_CPU(Thread_Base *, clean_server_thread_ptr);
/* EXIT_NOTIFY jobs stay per-CPU: each listen thread reaps what it spawned. */
DEFINE_PER_CPU(struct list_entry, clean_exit_notify_jobs);
DEFINE_PER_CPU(bool, clean_exit_notify_jobs_ready);

static char clean_server_thread_name[] = "clean_server_thread";

/* Shared clean_listen: one port, one service_id, creator pin once. */
static u16 clean_server_service_id;
static bool clean_server_service_id_valid;
static Message_Port_t *clean_server_port_owned;

static i64 clean_claim_and_delete_task(pid_t pid);

/*
 * Protocol: doc/linux_compat/protocols/EXIT_CLEAN.md
 * Port names: doc/linux_compat/protocols/PORT_NAMING.md §4
 *
 * Global clean_listen + one coop thread per CPU (all recv the same port):
 *   THREAD_REAP / TASK_REAP*  — on whichever clean thread wins recv
 *   EXIT_NOTIFY              — try_send + park on listen (no gen_thread);
 *                              poll retries; yield instead of blocking recv
 *                              while jobs remain so parent can enter wait4
 *
 * Cross-CPU by design: core0's reap may run on core1. Forbidden is N private
 * ports / pending-pool lies — not "N threads on one port".
 *
 * THREAD_REAP zombie wait stays inline (schedule + ready→zombie promote).
 * Parking THREAD_REAP itself without guaranteed EXIT_NOTIFY hung ash wait4.
 */

typedef struct clean_exit_notify_job {
        struct list_entry node;
        pid_t ppid;
        pid_t child_pid;
        i32 exit_code;
} clean_exit_notify_job_t;

static void clean_exit_notify_fallback_pending(pid_t ppid, pid_t child_pid,
                                               i32 exit_code);

static void clean_exit_notify_jobs_ensure(void)
{
        if (percpu(clean_exit_notify_jobs_ready))
                return;
        INIT_LIST_HEAD(&percpu(clean_exit_notify_jobs));
        percpu(clean_exit_notify_jobs_ready) = true;
}

static void clean_exit_notify_job_free(clean_exit_notify_job_t *job)
{
        struct allocator *alloc = percpu(kallocator);

        if (!job)
                return;
        list_del_init(&job->node);
        if (alloc && alloc->m_free)
                alloc->m_free(alloc, job);
}

/*
 * Coop poll: retry parked EXIT_NOTIFY via try_deliver. Never schedule() here
 * (coop_loop yields when we return true). Returns true if jobs remain.
 */
static bool clean_poll_exit_notify_jobs(void)
{
        struct list_entry *pos;
        struct list_entry *n;

        clean_exit_notify_jobs_ensure();
        list_for_each_safe(pos, n, &percpu(clean_exit_notify_jobs))
        {
                clean_exit_notify_job_t *job =
                        list_entry(pos, clean_exit_notify_job_t, node);
                linux_proc_try_result_t tr;

                if (!find_task_by_pid(job->ppid)) {
                        /* Parent gone — drop; reparent/orphan paths elsewhere. */
                        clean_exit_notify_job_free(job);
                        continue;
                }

                tr = linux_proc_try_post_exit_notify(
                        job->ppid, job->child_pid, job->exit_code);
                if (tr == LINUX_PROC_TRY_DELIVERED) {
                        clean_exit_notify_job_free(job);
                        continue;
                }
                if (tr == LINUX_PROC_TRY_FAIL) {
                        clean_exit_notify_fallback_pending(
                                job->ppid, job->child_pid, job->exit_code);
                        clean_exit_notify_job_free(job);
                        continue;
                }
                /* AGAIN: keep parked */
        }

        return !list_empty(&percpu(clean_exit_notify_jobs));
}

/*
 * Alloc/OOM / hard fail: push pending_exits + poke (WAIT_INTERRUPT →
 * try_pending). Must not block listen on wait_port.
 */
static void clean_exit_notify_fallback_pending(pid_t ppid, pid_t child_pid,
                                               i32 exit_code)
{
        Tcb_Base *parent;
        linux_proc_append_t *parent_pa;

        pr_warn("[CLEAN] EXIT_NOTIFY fallback pending+poke ppid=%d child=%d\n",
                (int)ppid,
                (int)child_pid);

        parent = find_task_by_pid(ppid);
        if (!parent)
                return;
        parent_pa = linux_proc_append(parent);
        if (!parent_pa)
                return;
        if (!linux_proc_wait_pending_push(parent_pa, child_pid, exit_code)) {
                pr_error(
                        "[clean_server] EXIT_NOTIFY fallback pending_push failed pid=%d\n",
                        (int)child_pid);
                return;
        }
        (void)linux_proc_wait_poke(ppid);
}

/*
 * Link A: try_send EXIT_NOTIFY on listen; park on AGAIN so this thread can
 * still accept TASK_REAP_SYNC. No one-shot worker thread.
 */
static void clean_async_exit_notify(pid_t ppid, pid_t child_pid, i32 exit_code)
{
        struct allocator *alloc = percpu(kallocator);
        clean_exit_notify_job_t *job;
        linux_proc_try_result_t tr;

        (void)clean_poll_exit_notify_jobs();

        if (ppid <= 0 || child_pid <= 0) {
                clean_exit_notify_fallback_pending(ppid, child_pid, exit_code);
                return;
        }

        tr = linux_proc_try_post_exit_notify(ppid, child_pid, exit_code);
        if (tr == LINUX_PROC_TRY_DELIVERED)
                return;
        if (tr == LINUX_PROC_TRY_FAIL) {
                clean_exit_notify_fallback_pending(ppid, child_pid, exit_code);
                return;
        }

        if (!alloc || !alloc->m_alloc) {
                clean_exit_notify_fallback_pending(ppid, child_pid, exit_code);
                return;
        }

        job = (clean_exit_notify_job_t *)alloc->m_alloc(alloc, sizeof(*job));
        if (!job) {
                pr_error("[clean_server] EXIT_NOTIFY job alloc failed\n");
                clean_exit_notify_fallback_pending(ppid, child_pid, exit_code);
                return;
        }
        memset(job, 0, sizeof(*job));
        INIT_LIST_HEAD(&job->node);
        job->ppid = ppid;
        job->child_pid = child_pid;
        job->exit_code = exit_code;
        clean_exit_notify_jobs_ensure();
        list_add_tail(&job->node, &percpu(clean_exit_notify_jobs));
}

/*
 * Handshake (EXIT_CLEAN): after THREAD_REAP rendezvous the exitor may be
 * ready but not yet scheduled to store zombie. Promote ready→zombie; only
 * schedule while still blocked on IPC or running on another CPU.
 */
static bool clean_wait_exitor_zombie(Thread_Base *target)
{
        if (!target || !(target->flags & THREAD_FLAG_EXIT_REQUESTED)) {
                return target
                       && thread_get_status(target) == thread_status_zombie;
        }

        for (;;) {
                u64 st = thread_get_status(target);

                if (st == thread_status_zombie) {
                        return true;
                }
                if (st != thread_status_block_on_send
                    && st != thread_status_block_on_receive
                    && st == thread_status_ready) {
                        (void)thread_set_status(target, thread_status_zombie);
                        return true;
                }
                schedule(percpu(core_tm));
        }
}

static void clean_handle_thread_reap(const kmsg_t *km)
{
        void *vthread;
        i64 exit_code;
        enum {
                REAP_NONE = 0,
                REAP_LINK_B, /* REAPED → claim + delete_task */
                REAP_LINK_A, /* ZOMBIE → async EXIT_NOTIFY */
        } after = REAP_NONE;
        pid_t task_pid = 0;
        pid_t ppid = 0;
        i32 notify_exit_code = 0;

        if (ipc_serial_decode(km->payload,
                              km->hdr.payload_len,
                              LINUX_KMSG_FMT_THREAD_REAP,
                              &vthread,
                              &exit_code)
            != REND_SUCCESS) {
                pr_error("[clean_server] THREAD_REAP: decode failed\n");
                return;
        }

        Thread_Base *target = (Thread_Base *)vthread;
        Tcb_Base *task = target ? target->belong_tcb : NULL;

        if (!target) {
                pr_error("[clean_server] THREAD_REAP: NULL thread\n");
                return;
        }

        Thread_Base *curr = get_cpu_current_thread();
        if (target == curr) {
                pr_error(
                        "[clean_server] THREAD_REAP: cannot reap current thread\n");
                return;
        }

        if (target == percpu(boot_thread_ptr)
            || target == percpu(idle_thread_ptr)) {
                pr_error("[clean_server] THREAD_REAP: init/idle blocked\n");
                return;
        }

        if (!clean_wait_exitor_zombie(target)) {
                pr_error(
                        "[clean_server] THREAD_REAP: not zombie (status=%lu)\n",
                        thread_get_status(target));
                return;
        }

#ifdef LINUX_COMPAT_TEST
        {
                /*
                 * Path-B PID1 wait cookie (usually /init). Suite ELFs under
                 * ash run_all are Link A — progress depends on EXIT_NOTIFY,
                 * not this cookie.
                 */
                linux_thread_append_t *ta = linux_thread_append(target);

                if (ta && ta->boot_wait_cookie != 0 && target->tm) {
                        linux_boot_notify_exit((i32)target->tm->owner_cpu,
                                               ta->boot_wait_cookie,
                                               exit_code);
                }
        }
#endif

        {
                error_t e = delete_thread(target);

                if (e != REND_SUCCESS) {
                        pr_error("[clean_server] THREAD_REAP: delete_thread "
                                 "failed e=%d\n",
                                 (int)e);
                        return;
                }
        }

        if (!task) {
                return;
        }

        {
                linux_proc_append_t *pa = linux_proc_append(task);

                lock_cas(&task->thread_list_lock);
                if (task->thread_number == 0 && pa) {
                        if (pa->exit_state == LINUX_EXIT_REAPED) {
                                /* Link B: orphan / no wait reaper. */
                                after = REAP_LINK_B;
                                task_pid = task->pid;
                        } else if (pa->exit_state == LINUX_EXIT_ZOMBIE
                                   && !pa->exit_notify_sent) {
                                /* Candidate Link A; confirm parent unlocked. */
                                after = REAP_LINK_A;
                                task_pid = task->pid;
                                ppid = pa->ppid;
                                notify_exit_code = pa->exit_code;
                        }
                }
                unlock_cas(&task->thread_list_lock);

                if (after == REAP_LINK_A) {
                        /*
                         * Parent gone since sys_exit → demote to Link B.
                         * Never set exit_notify_sent then skip notify (zombie
                         * would be unreapable).
                         */
                        if (ppid > 0 && find_task_by_pid(ppid)) {
                                lock_cas(&task->thread_list_lock);
                                if (pa->exit_state == LINUX_EXIT_ZOMBIE
                                    && !pa->exit_notify_sent) {
                                        pa->exit_notify_sent = 1;
                                } else {
                                        after = REAP_NONE;
                                }
                                unlock_cas(&task->thread_list_lock);
                        } else {
                                lock_cas(&task->thread_list_lock);
                                if (pa->exit_state == LINUX_EXIT_ZOMBIE) {
                                        pa->exit_state = LINUX_EXIT_REAPED;
                                        after = REAP_LINK_B;
                                } else {
                                        after = REAP_NONE;
                                }
                                unlock_cas(&task->thread_list_lock);
                        }
                }
        }

        if (after == REAP_LINK_B && task_pid > 0) {
                (void)clean_claim_and_delete_task(task_pid);
                return;
        }

        if (after == REAP_LINK_A && task_pid > 0) {
                /*
                 * Must return to listen after spawn — parent wait4 will
                 * TASK_REAP_SYNC on the shared clean_listen.
                 */
                clean_async_exit_notify(ppid, task_pid, notify_exit_code);
        }
}

static i64 clean_claim_and_delete_task(pid_t pid)
{
        Tcb_Base *task;
        linux_proc_append_t *pa;

        if (pid <= 0)
                return -LINUX_EINVAL;

        for (;;) {
                task = find_task_by_pid(pid);
                if (!task)
                        return 0;

                pa = linux_proc_append(task);
                if (!pa)
                        return -LINUX_ECHILD;

                lock_cas(&task->thread_list_lock);
                if (task->thread_number != 0) {
                        unlock_cas(&task->thread_list_lock);
                        schedule(percpu(core_tm));
                        continue;
                }
                if (pa->exit_state == LINUX_EXIT_TASK_CLAIMED) {
                        unlock_cas(&task->thread_list_lock);
                        schedule(percpu(core_tm));
                        continue;
                }
                if (pa->exit_state != LINUX_EXIT_REAPED) {
                        unlock_cas(&task->thread_list_lock);
                        return -LINUX_ECHILD;
                }
                pa->exit_state = LINUX_EXIT_TASK_CLAIMED;
                unlock_cas(&task->thread_list_lock);
                break;
        }

        if (task->vs == &root_vspace) {
                pr_error(
                        "[ Error ] TASK_REAP: user task must not use root vspace\n");
        }

        {
                error_t e = delete_task(task);

                if (e != REND_SUCCESS) {
                        pr_error(
                                "[clean_server] delete_task failed (task=%p, e=%d)\n",
                                (void *)task,
                                (int)e);
                        return -LINUX_EAGAIN;
                }
        }
        return 0;
}

static void clean_handle_task_reap(const kmsg_t *km, const char *reply_port,
                                   bool want_reply)
{
        i32 pid;
        i64 result = 0;

        if (want_reply) {
                char *rp = NULL;

                if (ipc_serial_decode(km->payload,
                                      km->hdr.payload_len,
                                      LINUX_KMSG_FMT_TASK_REAP_SYNC,
                                      &pid,
                                      &rp)
                    != REND_SUCCESS) {
                        pr_error(
                                "[clean_server] TASK_REAP_SYNC: decode failed\n");
                        ipc_rpc_reply(km,
                                      reply_port,
                                      clean_server_service_id,
                                      IPC_RPC_RESP_OPCODE_DEFAULT,
                                      IPC_RPC_RESP_FMT_DEFAULT,
                                      -LINUX_EIO);
                        return;
                }
                reply_port = rp;
        } else if (ipc_serial_decode(km->payload,
                                     km->hdr.payload_len,
                                     LINUX_KMSG_FMT_TASK_REAP,
                                     &pid)
                   != REND_SUCCESS) {
                pr_error("[clean_server] TASK_REAP: decode failed\n");
                return;
        }

        if (pid <= 0) {
                pr_error("[clean_server] TASK_REAP: invalid pid=%d\n",
                         (int)pid);
                if (want_reply) {
                        ipc_rpc_reply(km,
                                      reply_port,
                                      clean_server_service_id,
                                      IPC_RPC_RESP_OPCODE_DEFAULT,
                                      IPC_RPC_RESP_FMT_DEFAULT,
                                      -LINUX_EINVAL);
                }
                return;
        }

        result = clean_claim_and_delete_task((pid_t)pid);
        if (result == -LINUX_ECHILD) {
                pr_error(
                        "[clean_server] TASK_REAP: pid=%d bad exit_state or append\n",
                        (int)pid);
        }

        if (want_reply) {
                ipc_rpc_reply(km,
                              reply_port,
                              clean_server_service_id,
                              IPC_RPC_RESP_OPCODE_DEFAULT,
                              IPC_RPC_RESP_FMT_DEFAULT,
                              result);
        }
}

static void clean_handle_message(Message_t *msg)
{
        const kmsg_t *km;

        (void)clean_poll_exit_notify_jobs();

        if (!msg || !msg->data) {
                pr_error("[clean_server] NULL msg or msg->data\n");
                return;
        }

        km = kmsg_from_msg(msg);
        if (!km) {
                pr_error("[clean_server] NULL kmsg\n");
                return;
        }

        if (km->hdr.module != clean_server_service_id) {
                pr_error("[clean_server] Invalid kmsg module\n");
                return;
        }

        switch (km->hdr.opcode) {
        case KMSG_OP_CLEAN_THREAD_REAP:
                clean_handle_thread_reap(km);
                return;
        case KMSG_OP_CLEAN_TASK_REAP:
                clean_handle_task_reap(km, NULL, false);
                return;
        case KMSG_OP_CLEAN_TASK_REAP_SYNC:
                clean_handle_task_reap(km, NULL, true);
                return;
        default:
                pr_error("[clean_server] Unknown opcode %u\n",
                         (unsigned)km->hdr.opcode);
        }
}

static void clean_server_on_message(Message_t *msg, u16 service_id)
{
        (void)service_id;
        clean_handle_message(msg);
}

/*
 * Shared clean_listen: any CPU may create/register (SMP init races).
 * Creator pin stays in clean_server_port_owned (first successful register).
 */
static void clean_server_ensure_port(void)
{
        Message_Port_t *port;
        error_t err;

        if (!global_port_table) {
                pr_error(
                        "[clean_server] global_port_table not initialized (cpu=%lu)\n",
                        (u64)percpu(cpu_number));
                return;
        }

        if (clean_server_service_id_valid)
                return;

        port = port_table_lookup(global_port_table, CLEAN_SERVER_PORT_NAME);
        if (port) {
                clean_server_service_id = port->service_id;
                clean_server_service_id_valid = true;
                ref_put(&port->refcount, free_message_port_ref);
                return;
        }

        port = create_message_port(CLEAN_SERVER_PORT_NAME);
        if (!port) {
                pr_error("[clean_server] failed to create message port\n");
                return;
        }

        err = register_port(global_port_table, port);
        if (err != REND_SUCCESS) {
                delete_message_port_structure(port);
                port = port_table_lookup(global_port_table,
                                         CLEAN_SERVER_PORT_NAME);
                if (!port) {
                        pr_error(
                                "[clean_server] register_port failed e=%d and lookup empty\n",
                                (int)err);
                        return;
                }
                clean_server_service_id = port->service_id;
                clean_server_service_id_valid = true;
                ref_put(&port->refcount, free_message_port_ref);
                return;
        }

        clean_server_port_owned = port;
        clean_server_service_id = port->service_id;
        clean_server_service_id_valid = true;
        pr_info("[clean_server] registered '%s' service_id=%u (cpu=%lu)\n",
                CLEAN_SERVER_PORT_NAME,
                (unsigned)port->service_id,
                (u64)percpu(cpu_number));
}

static bool clean_server_poll_pending(void *ctx)
{
        (void)ctx;
        /* Retry parked EXIT_NOTIFY; true → coop_loop yields instead of recv. */
        return clean_poll_exit_notify_jobs();
}

void clean_server_thread(void)
{
        clean_server_ensure_port();
        clean_exit_notify_jobs_ensure();
        ipc_server_coop_loop(CLEAN_SERVER_PORT_NAME,
                             clean_server_on_message,
                             clean_server_poll_pending,
                             NULL);
}

static void clean_server_init(void)
{
        /*
         * One global port (clean_listen); one coop thread per CPU, all recv
         * that port so reaps can run cross-CPU. DEFINE_INIT runs on BSP and
         * each AP — do not collapse to BSP-only.
         */
        clean_server_ensure_port();

        if (!clean_server_service_id_valid) {
                pr_error("[clean_server] clean_listen not available (cpu=%lu)\n",
                         (u64)percpu(cpu_number));
                return;
        }

        {
                error_t e =
                        gen_thread_from_func(&percpu(clean_server_thread_ptr),
                                             (kthread_func)clean_server_thread,
                                             clean_server_thread_name,
                                             percpu(core_tm),
                                             NULL);
                if (e != REND_SUCCESS) {
                        pr_error("[ Error ]clean server init fail (cpu=%lu e=%d)\n",
                                 (u64)percpu(cpu_number),
                                 (int)e);
                }
        }
}
DEFINE_INIT(clean_server_init);
