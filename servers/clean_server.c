#include <modules/log/log.h>
#include <common/types.h>
#include <common/string.h>
#include <common/dsa/list.h>
#include <common/refcount.h>
#include <rendezvos/ipc/ipc_serial.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/task/thread.h>
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

/*
 * Protocol: doc/linux_compat/protocols/EXIT_CLEAN.md (v2)
 *   THREAD_REAP — inline delete_thread; Link B linux_proc_reap; Link A EXIT_NOTIFY
 */

typedef struct clean_exit_notify_job {
        struct list_entry node;
        pid_t ppid;
        linux_proc_resource_t *child;
        i32 exit_code;
} clean_exit_notify_job_t;

static void clean_exit_notify_fallback_pending(pid_t ppid,
                                               linux_proc_resource_t *child,
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

                if (!find_proc_by_pid(job->ppid)) {
                        /* Parent gone — drop; reparent/orphan paths elsewhere. */
                        clean_exit_notify_job_free(job);
                        continue;
                }

                tr = linux_proc_try_post_exit_notify(
                        job->ppid, job->child, job->exit_code);
                if (tr == LINUX_PROC_TRY_DELIVERED) {
                        job->child = NULL;
                        clean_exit_notify_job_free(job);
                        continue;
                }
                if (tr == LINUX_PROC_TRY_FAIL) {
                        clean_exit_notify_fallback_pending(
                                job->ppid, job->child, job->exit_code);
                        job->child = NULL;
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
static void clean_exit_notify_fallback_pending(pid_t ppid,
                                               linux_proc_resource_t *child,
                                               i32 exit_code)
{
        linux_proc_resource_t *parent;

        if (!child)
                return;

        pr_warn("[CLEAN] EXIT_NOTIFY fallback pending+poke ppid=%d child=%d\n",
                (int)ppid,
                (int)child->pid);

        parent = find_proc_by_pid(ppid);
        if (!parent)
                return;
        if (!linux_proc_wait_pending_push(parent, child, exit_code)) {
                pr_error(
                        "[clean_server] EXIT_NOTIFY fallback pending_push failed pid=%d\n",
                        (int)child->pid);
                return;
        }
        (void)linux_proc_wait_poke(ppid);
}

/*
 * Link A: try_send EXIT_NOTIFY with proc* in payload; park on AGAIN.
 */
static void clean_async_exit_notify(pid_t ppid, linux_proc_resource_t *child,
                                    i32 exit_code)
{
        struct allocator *alloc = percpu(kallocator);
        clean_exit_notify_job_t *job;
        linux_proc_try_result_t tr;

        (void)clean_poll_exit_notify_jobs();

        if (ppid <= 0 || !child || child->pid <= 0)
                return;

        tr = linux_proc_try_post_exit_notify(ppid, child, exit_code);
        if (tr == LINUX_PROC_TRY_DELIVERED)
                return;
        if (tr == LINUX_PROC_TRY_FAIL) {
                clean_exit_notify_fallback_pending(ppid, child, exit_code);
                return;
        }

        if (!alloc || !alloc->m_alloc) {
                clean_exit_notify_fallback_pending(ppid, child, exit_code);
                return;
        }

        job = (clean_exit_notify_job_t *)alloc->m_alloc(alloc, sizeof(*job));
        if (!job) {
                pr_error("[clean_server] EXIT_NOTIFY job alloc failed\n");
                clean_exit_notify_fallback_pending(ppid, child, exit_code);
                return;
        }
        memset(job, 0, sizeof(*job));
        INIT_LIST_HEAD(&job->node);
        job->ppid = ppid;
        job->child = child;
        job->exit_code = exit_code;
        clean_exit_notify_jobs_ensure();
        list_add_tail(&job->node, &percpu(clean_exit_notify_jobs));
}

/*
 * After sync detach: false if other non-exiting threads remain. If sys_exit
 * hinted exit_last_thread, wait for sibling exitors. When thread_number==0,
 * set exit_last_thread (authoritative) for wait4.
 */
static bool clean_proc_bundle_detached(linux_proc_resource_t *proc)
{
        if (!proc)
                return false;

        lock_cas(&proc->thread_list_lock);
        if (proc->thread_number > 0) {
                bool last_hint = proc->exit_last_thread;

                unlock_cas(&proc->thread_list_lock);
                if (!last_hint)
                        return false;
                linux_proc_wait_all_threads_detached(proc);
        } else {
                unlock_cas(&proc->thread_list_lock);
        }

        lock_cas(&proc->thread_list_lock);
        if (proc->thread_number != 0) {
                unlock_cas(&proc->thread_list_lock);
                return false;
        }
        proc->exit_last_thread = 1;
        unlock_cas(&proc->thread_list_lock);
        return true;
}

/*
 * THREAD_REAP handler: exitor sent reap before zombie; promote if needed so
 * delete_thread can run (send may return before zombie store is visible).
 */
static void clean_handle_thread_reap(const kmsg_t *km)
{
        void *vthread;
        i64 exit_code;
        enum {
                REAP_NONE = 0,
                REAP_LINK_B,
                REAP_LINK_A,
        } after = REAP_NONE;
        pid_t notify_ppid = 0;
        i32 notify_exit_code = 0;
        Thread_Base *target;
        linux_proc_resource_t *proc = NULL;
        error_t del_e;
        u64 st;

        if (ipc_serial_decode(km->payload,
                              km->hdr.payload_len,
                              LINUX_KMSG_FMT_THREAD_REAP,
                              &vthread,
                              &exit_code)
            != REND_SUCCESS) {
                pr_error("[clean_server] THREAD_REAP: decode failed\n");
                return;
        }

        target = (Thread_Base *)vthread;
        if (!target) {
                pr_error("[clean_server] THREAD_REAP: NULL thread\n");
                return;
        }

        if (target == get_cpu_current_thread()) {
                pr_error(
                        "[clean_server] THREAD_REAP: cannot reap current thread\n");
                return;
        }

        if (target == percpu(boot_thread_ptr)
            || target == percpu(idle_thread_ptr)) {
                pr_error("[clean_server] THREAD_REAP: init/idle blocked\n");
                return;
        }

        if (!(target->flags & THREAD_FLAG_EXIT_REQUESTED)
            && thread_get_status(target) != thread_status_zombie) {
                pr_error(
                        "[clean_server] THREAD_REAP: not zombie (status=%lu)\n",
                        thread_get_status(target));
                return;
        }

        st = thread_get_status(target);
        if (st != thread_status_zombie)
                (void)thread_set_status(target, thread_status_zombie);

        if (target->vs == &root_vspace) {
                pr_error(
                        "[clean_server] THREAD_REAP: user thread must not use root vspace\n");
                return;
        }

        proc = linux_proc_of(target);
        if (proc && !ref_get_not_zero(&proc->refcount))
                proc = NULL;

#ifdef LINUX_COMPAT_TEST
        {
                linux_thread_append_t *ta = linux_thread_append(target);

                if (ta && ta->boot_wait_cookie != 0 && target->tm) {
                        linux_boot_notify_exit((i32)target->tm->owner_cpu,
                                               ta->boot_wait_cookie,
                                               exit_code);
                }
        }
#endif

        del_e = delete_thread(target);
        if (del_e != REND_SUCCESS) {
                pr_error("[clean_server] THREAD_REAP: delete_thread failed e=%d\n",
                         (int)del_e);
                if (proc)
                        (void)linux_proc_put(proc);
                return;
        }

        if (!proc)
                return;

        /*
         * Sync detach: fini/detach runs on last thread ref, which may lag
         * delete_thread while this handler still holds the IPC rendezvous ref.
         */
        linux_proc_detach_thread(target);

        if (!clean_proc_bundle_detached(proc)) {
                (void)linux_proc_put(proc);
                return;
        }

        lock_cas(&proc->thread_list_lock);
        if (proc->exit_state == LINUX_EXIT_CLAIMED
            || proc->exit_state == LINUX_EXIT_NOTIFIED) {
                /* Parent reaping or EXIT_NOTIFY already committed. */
        } else if (!proc_has_wait_reaper(proc)) {
                after = REAP_LINK_B;
        } else if (proc->exit_state == LINUX_EXIT_ZOMBIE) {
                after = REAP_LINK_A;
                notify_ppid = proc->ppid;
                notify_exit_code = proc->exit_code;
        }
        unlock_cas(&proc->thread_list_lock);

        if (after == REAP_LINK_A) {
                if (notify_ppid <= 0 || !find_proc_by_pid(notify_ppid)) {
                        after = REAP_LINK_B;
                } else {
                        lock_cas(&proc->thread_list_lock);
                        if (proc->exit_state == LINUX_EXIT_ZOMBIE)
                                proc->exit_state = LINUX_EXIT_NOTIFIED;
                        else
                                after = REAP_NONE;
                        unlock_cas(&proc->thread_list_lock);
                }
        }

        if (after == REAP_LINK_B) {
                if (linux_proc_reap(proc) != REND_SUCCESS) {
                        pr_error("[clean_server] Link B linux_proc_reap failed pid=%d\n",
                                 (int)proc->pid);
                }
                (void)linux_proc_put(proc);
                return;
        }

        if (after == REAP_LINK_A)
                clean_async_exit_notify(notify_ppid, proc, notify_exit_code);
        (void)linux_proc_put(proc);
}

static void clean_handle_message(Message_t *msg, u16 service_id)
{
        const kmsg_t *km;

        (void)service_id;
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

        if (km->hdr.opcode == KMSG_OP_CLEAN_THREAD_REAP) {
                clean_handle_thread_reap(km);
                return;
        }

        pr_error("[clean_server] Unknown opcode %u\n",
                 (unsigned)km->hdr.opcode);
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

        port = create_message_port(CLEAN_SERVER_PORT_NAME, NULL);
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
        return clean_poll_exit_notify_jobs();
}

void clean_server_thread(void)
{
        clean_server_ensure_port();
        clean_exit_notify_jobs_ensure();
        ipc_server_coop_loop(CLEAN_SERVER_PORT_NAME,
                             clean_handle_message,
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
