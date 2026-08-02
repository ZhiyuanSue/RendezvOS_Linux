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
#include <linux_compat/initcall.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/proc/wait_ipc.h>
#include <linux_compat/test_sync_ipc.h>
#ifdef LINUX_COMPAT_TEST
#include <linux_compat/test_runner.h>
#endif

extern struct Port_Table *global_port_table;

DEFINE_PER_CPU(Thread_Base *, clean_server_thread_ptr);

static char clean_server_thread_name[] = "clean_server_thread";

static u16 clean_server_service_id;
static bool clean_server_service_id_valid;
/* Creator pin: keeps the listen port alive for the life of the kernel. */
static Message_Port_t *clean_server_port_owned;
static bool clean_listen_started;

static i64 clean_claim_and_delete_task(pid_t pid);

/*
 * Protocol: doc/linux_compat/protocols/EXIT_CLEAN.md
 *
 * Single global clean_listen (ipc_server_coop_loop):
 *   THREAD_REAP / TASK_REAP*  — on listen (not a worker pool)
 *   EXIT_NOTIFY              — one-shot only (must not block listen on
 *                              wait_port; parent needs TASK_REAP_SYNC)
 *
 * THREAD_REAP zombie wait stays inline (schedule + ready→zombie promote).
 * Parking it left Link A children without EXIT_NOTIFY → ash wait4 hung
 * after the first run_all test (END printed, next test never started).
 */

typedef struct clean_exit_notify_job {
        struct list_entry node;
        Thread_Base *thread;
        char *thread_name;
        pid_t ppid;
        pid_t child_pid;
        i32 exit_code;
        volatile bool finished;
} clean_exit_notify_job_t;

static struct list_entry clean_exit_notify_jobs;
static bool clean_exit_notify_jobs_ready;

static void clean_exit_notify_fallback_pending(pid_t ppid, pid_t child_pid,
                                               i32 exit_code);

static void clean_exit_notify_jobs_ensure(void)
{
        if (clean_exit_notify_jobs_ready)
                return;
        INIT_LIST_HEAD(&clean_exit_notify_jobs);
        clean_exit_notify_jobs_ready = true;
}

/*
 * Non-blocking: never schedule() here (would wedge coop listen the same way
 * parking THREAD_REAP did). Promote ready→zombie like THREAD_REAP; return
 * true while a finished worker still needs another poll.
 */
static bool clean_poll_exit_notify_jobs(void)
{
        struct list_entry *pos;
        struct list_entry *n;
        struct allocator *alloc = percpu(kallocator);
        bool still_pending = false;

        clean_exit_notify_jobs_ensure();
        list_for_each_safe(pos, n, &clean_exit_notify_jobs) {
                clean_exit_notify_job_t *job =
                        list_entry(pos, clean_exit_notify_job_t, node);
                Thread_Base *thr;
                u64 st;

                /*
                 * In-flight workers (!finished) block on wait_port in their
                 * own threads — do not busy-spin the listen loop on them.
                 */
                if (!job->finished)
                        continue;
                thr = job->thread;
                if (!thr) {
                        list_del_init(&job->node);
                        if (alloc && alloc->m_free)
                                alloc->m_free(alloc, job);
                        continue;
                }

                st = thread_get_status(thr);
                if (st != thread_status_zombie) {
                        if ((thr->flags & THREAD_FLAG_EXIT_REQUESTED)
                            && st == thread_status_ready) {
                                (void)thread_set_status(thr,
                                                        thread_status_zombie);
                        } else {
                                still_pending = true;
                                continue;
                        }
                }

                list_del_init(&job->node);
                if (delete_thread(thr) != REND_SUCCESS) {
                        pr_error(
                                "[clean_server] EXIT_NOTIFY worker delete failed\n");
                }
                job->thread = NULL;
                job->thread_name = NULL; /* freed in delete_thread */
                if (alloc && alloc->m_free)
                        alloc->m_free(alloc, job);
        }
        return still_pending;
}

static void *clean_exit_notify_thread(void *arg)
{
        clean_exit_notify_job_t *job = (clean_exit_notify_job_t *)arg;
        Thread_Base *self = get_cpu_current_thread();

        if (job) {
                /*
                 * Must not drop EXIT_NOTIFY: parent wait4 would hang on a
                 * zombie. If blocking deliver fails after retries, fall back
                 * to pending_exits + poke (same as spawn/alloc failure).
                 */
                if (!linux_proc_post_exit_notify(
                            job->ppid, job->child_pid, job->exit_code)) {
                        clean_exit_notify_fallback_pending(
                                job->ppid, job->child_pid, job->exit_code);
                }
                job->finished = true;
        }
        if (self) {
                thread_or_flags(self, THREAD_FLAG_EXIT_REQUESTED);
                (void)thread_set_status(self, thread_status_zombie);
        }
        for (;;)
                schedule(percpu(core_tm));
        return NULL;
}

/*
 * Link A only: must not block clean_listen on wait_port (parent may need to
 * send TASK_REAP_SYNC to the same listen). One-shot worker.
 *
 * If spawn/alloc fails: push pending_exits on the parent and poke wait_port
 * (WAIT_INTERRUPT → try_pending). See protocols/EXIT_CLEAN.md.
 */
static void clean_exit_notify_fallback_pending(pid_t ppid, pid_t child_pid,
                                               i32 exit_code)
{
        Tcb_Base *parent;
        linux_proc_append_t *parent_pa;

        pr_warn(
                "[CLEAN] EXIT_NOTIFY fallback pending+poke ppid=%d child=%d\n",
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

static void clean_async_exit_notify(pid_t ppid, pid_t child_pid, i32 exit_code)
{
        struct allocator *alloc = percpu(kallocator);
        clean_exit_notify_job_t *job;
        Thread_Base *thr = NULL;
        char *name;
        error_t e;

        (void)clean_poll_exit_notify_jobs();

        if (!alloc || !alloc->m_alloc || ppid <= 0 || child_pid <= 0) {
                clean_exit_notify_fallback_pending(ppid, child_pid, exit_code);
                return;
        }

        job = (clean_exit_notify_job_t *)alloc->m_alloc(alloc, sizeof(*job));
        name = (char *)alloc->m_alloc(alloc, 32);
        if (!job || !name) {
                if (job && alloc->m_free)
                        alloc->m_free(alloc, job);
                if (name && alloc->m_free)
                        alloc->m_free(alloc, name);
                pr_error("[clean_server] EXIT_NOTIFY job alloc failed\n");
                clean_exit_notify_fallback_pending(ppid, child_pid, exit_code);
                return;
        }
        memset(job, 0, sizeof(*job));
        INIT_LIST_HEAD(&job->node);
        job->ppid = ppid;
        job->child_pid = child_pid;
        job->exit_code = exit_code;
        {
                const char *p = "clean_exit_ntf";
                u32 i = 0;

                while (p[i] && i + 1 < 32) {
                        name[i] = p[i];
                        i++;
                }
                name[i] = '\0';
        }
        job->thread_name = name;

        list_add_tail(&job->node, &clean_exit_notify_jobs);
        e = gen_thread_from_func(&thr,
                                 clean_exit_notify_thread,
                                 name,
                                 percpu(core_tm),
                                 job);
        if (e != REND_SUCCESS || !thr) {
                list_del_init(&job->node);
                if (alloc->m_free) {
                        alloc->m_free(alloc, name);
                        alloc->m_free(alloc, job);
                }
                pr_error("[clean_server] EXIT_NOTIFY spawn failed e=%d\n",
                         (int)e);
                clean_exit_notify_fallback_pending(ppid, child_pid, exit_code);
                return;
        }
        job->thread = thr;
}

/*
 * Handshake (EXIT_CLEAN): after THREAD_REAP rendezvous the exitor may be
 * ready but not yet scheduled to store zombie. Promote ready→zombie; only
 * schedule while still blocked on IPC or running on another CPU.
 */
static bool clean_wait_exitor_zombie(Thread_Base *target)
{
        if (!target || !(target->flags & THREAD_FLAG_EXIT_REQUESTED)) {
                return target && thread_get_status(target) == thread_status_zombie;
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

        if (target == percpu(init_thread_ptr)
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
                 * Boot harness cookie (usually /init). Per-test ELFs under
                 * ash run_all are Link A — progress depends on EXIT_NOTIFY,
                 * not this cookie.
                 */
                linux_thread_append_t *ta = linux_thread_append(target);

                if (ta && ta->test_cookie != 0 && target->tm) {
                        linux_user_test_notify_exit((i32)target->tm->owner_cpu,
                                                    ta->test_cookie,
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
                 * TASK_REAP_SYNC on the same clean_listen.
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
                pr_error("[clean_server] TASK_REAP: invalid pid=%d\n", (int)pid);
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
 * Global listen port: any CPU may create/register (SMP init races).
 * Keep creator ref in clean_server_port_owned.
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
        /* Only EXIT_NOTIFY worker teardown is pending; THREAD_REAP is inline. */
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
        clean_server_ensure_port();

        /*
         * One global listen only. Every CPU spawning a pool listener on the
         * same clean_listen was a protocol violation (N private pools, one
         * port) and a root cause of "send done / no enter".
         */
        if (!linux_init_bsp_once(&clean_listen_started))
                return;

        if (!clean_server_service_id_valid) {
                pr_error("[clean_server] clean_listen not available on BSP\n");
                linux_init_bsp_mark_done(&clean_listen_started);
                return;
        }

        {
                error_t e = gen_thread_from_func(
                        &percpu(clean_server_thread_ptr),
                        (kthread_func)clean_server_thread,
                        clean_server_thread_name,
                        percpu(core_tm),
                        NULL);
                if (e != REND_SUCCESS) {
                        pr_error("[ Error ]clean server init fail (e=%d)\n",
                                 (int)e);
                }
        }
        linux_init_bsp_mark_done(&clean_listen_started);
}
DEFINE_INIT(clean_server_init);
