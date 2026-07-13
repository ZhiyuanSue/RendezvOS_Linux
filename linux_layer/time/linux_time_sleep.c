/*
 * Blocking sleep via core rendezvos_timer_event + per-thread IPC port.
 * See core/modules/test/single_timer_test.c and core/docs/ipc.md §10.
 *
 * recv_msg(sleep_port) waits for timer EXPIRE/CANCEL kmsg. Deliverable signals
 * are woken by linux_time_sleep_wake_for_signal() posting CANCEL to the same
 * port (signal_queue path); the sleeper then disarms the timer locally.
 */

#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/ipc/block_wake.h>
#include <linux_compat/ipc/rpc.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/signal/signal_deliver.h>
#include <linux_compat/time/linux_ktime.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/ipc_serial.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/kmsg_system.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/time.h>

extern struct Port_Table *global_port_table;

static tick_t linux_time_monotonic_to_arch_expiry(tick_t deadline_monotonic)
{
        tick_t now = rendezvos_time_now();
        tick_t arch_now = arch_timer_read();

        if (!time_before(now, deadline_monotonic)) {
                return arch_now;
        }
        return arch_now + (deadline_monotonic - now);
}

static size_t linux_time_sleep_port_name(char *buf, size_t bufsize, tid_t tid)
{
        const char *prefix = "linux_sleep_";
        size_t plen;
        size_t i;

        if (!buf || bufsize == 0) {
                return 0;
        }

        plen = strlen(prefix);
        if (plen >= bufsize) {
                buf[0] = '\0';
                return 0;
        }

        memcpy(buf, prefix, plen);
        i = plen;
        if (proc_format_pid(buf + i, bufsize - i, (pid_t)tid) == 0) {
                buf[0] = '\0';
                return 0;
        }
        return strlen(buf);
}

static Message_Port_t *linux_time_sleep_port_get_or_create(Thread_Base *thread)
{
        linux_thread_append_t *ta;
        char name[PORT_NAME_LEN_MAX];
        Message_Port_t *port;

        if (!thread || !global_port_table) {
                return NULL;
        }

        ta = linux_thread_append(thread);
        if (!ta) {
                return NULL;
        }
        if (ta->sleep_port) {
                return ta->sleep_port;
        }

        if (linux_time_sleep_port_name(name, sizeof(name), thread->tid) == 0) {
                return NULL;
        }

        port = ipc_rpc_port_lookup_or_create(name);
        if (!port) {
                return NULL;
        }

        ta->sleep_port = port;
        return port;
}

static bool linux_time_sleep_parse_kmsg(const Message_t *msg,
                                        Message_Port_t *port, u16 expect_opcode,
                                        u64 expect_token)
{
        const kmsg_t *km;
        i64 token;

        if (!msg || !port) {
                return false;
        }

        km = kmsg_from_msg(msg);
        if (!km || km->hdr.module != port->service_id
            || km->hdr.opcode != expect_opcode) {
                return false;
        }
        if (ipc_serial_decode(km->payload,
                              km->hdr.payload_len,
                              KMSG_FMT_SYSTEM_TIMER,
                              &token)
            != REND_SUCCESS) {
                return false;
        }
        return (u64)token == expect_token;
}

static void linux_time_sleep_drain_port_queue(void)
{
        Message_t *stale;

        while ((stale = dequeue_recv_msg()) != NULL) {
                ref_put(&stale->ms_queue_node.refcount, free_message_ref);
        }
}

static void linux_time_sleep_disarm(rendezvos_timer_event *event)
{
        if (!event) {
                return;
        }
        if (rendezvos_timer_event_exist(event)) {
                rendezvos_timer_event_cancel(event);
        }
        rendezvos_timer_event_fini(event);
}

static i64 linux_time_sleep_return_eintr(rendezvos_timer_event *event,
                                         tick_t deadline_count,
                                         linux_timespec_t *rem_out)
{
        tick_t now;

        linux_time_sleep_disarm(event);

        now = rendezvos_time_now();
        if (rem_out) {
                linux_ticks_to_timespec(deadline_count - now, rem_out);
        }
        return -LINUX_EINTR;
}

static bool linux_time_sleep_post_cancel(Message_Port_t *port, u64 token)
{
        Msg_Data_t *md;
        Message_t *msg;
        error_t err;

        if (!port) {
                return false;
        }

        md = kmsg_create(port->service_id,
                         KMSG_OP_SYSTEM_TIMER_CANCEL,
                         KMSG_FMT_SYSTEM_TIMER,
                         (i64)token);
        if (!md) {
                return false;
        }

        msg = create_message_with_msg(md);
        ref_put(&md->refcount, free_msgdata_ref_default);
        if (!msg) {
                return false;
        }

        err = ipc_system_try_deliver(port, msg, false);
        return err == REND_SUCCESS;
}

void linux_time_sleep_wake_for_signal(Thread_Base *thread)
{
        linux_thread_append_t *ta;
        Message_Port_t *port;

        if (!thread) {
                return;
        }

        ta = linux_thread_append(thread);
        if (!ta || !ta->sleep_port) {
                return;
        }
        if (thread_get_status(thread) != thread_status_block_on_receive) {
                return;
        }
        if ((Message_Port_t *)thread->port_ptr != ta->sleep_port) {
                return;
        }
        if (!linux_signal_thread_has_deliverable_pending(thread)) {
                return;
        }

        port = ta->sleep_port;
        (void)linux_time_sleep_post_cancel(port, ta->sleep_timer_token);
}

void linux_time_sleep_port_teardown(Thread_Base *thread)
{
        linux_thread_append_t *ta;
        char name[PORT_NAME_LEN_MAX];

        if (!thread || !global_port_table) {
                return;
        }

        ta = linux_thread_append(thread);
        if (!ta) {
                return;
        }

        if (!ta->sleep_port) {
                return;
        }

        linux_time_sleep_disarm(&ta->sleep_timer_event);

        if (linux_time_sleep_port_name(name, sizeof(name), thread->tid) != 0) {
                (void)unregister_port(global_port_table, name);
        }

        ta->sleep_port = NULL;
}

i64 linux_time_sleep_until_count(tick_t deadline_count,
                                 linux_timespec_t *rem_out)
{
        Thread_Base *thread = get_cpu_current_thread();
        Message_Port_t *port;
        linux_thread_append_t *ta;
        tick_t now;

        if (!thread) {
                return -LINUX_ESRCH;
        }

        port = linux_time_sleep_port_get_or_create(thread);
        if (!port) {
                return -LINUX_EAGAIN;
        }

        ta = linux_thread_append(thread);
        if (!ta) {
                return -LINUX_ESRCH;
        }

        linux_time_sleep_drain_port_queue();

        while (1) {
                rendezvos_timer_event *event = &ta->sleep_timer_event;
                u64 token;
                tick_t expires_at;
                error_t err;
                Message_t *msg;

                linux_time_sleep_disarm(event);

                now = rendezvos_time_now();
                if (!time_before(now, deadline_count)) {
                        return 0;
                }

                if (linux_signal_thread_has_deliverable_pending(thread)) {
                        if (rem_out) {
                                linux_ticks_to_timespec(deadline_count - now,
                                                        rem_out);
                        }
                        return -LINUX_EINTR;
                }

                token = ++ta->sleep_timer_token;

                err = rendezvos_timer_event_init(event, 0, port, token);
                if (err != REND_SUCCESS) {
                        schedule(percpu(core_tm));
                        continue;
                }

                expires_at =
                        linux_time_monotonic_to_arch_expiry(deadline_count);
                err = rendezvos_timer_event_add(event, expires_at);
                if (err != REND_SUCCESS) {
                        rendezvos_timer_event_fini(event);
                        schedule(percpu(core_tm));
                        continue;
                }

                err = recv_msg(port);

                if (err == -E_REND_PORT_CLOSED) {
                        linux_time_sleep_disarm(event);
                        return linux_time_sleep_return_eintr(
                                event, deadline_count, rem_out);
                }

                if (err != REND_SUCCESS) {
                        linux_time_sleep_disarm(event);
                        schedule(percpu(core_tm));
                        continue;
                }

                msg = dequeue_recv_msg();
                if (!msg) {
                        linux_time_sleep_disarm(event);
                        continue;
                }

                if (linux_signal_thread_has_deliverable_pending(thread)) {
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                        return linux_time_sleep_return_eintr(
                                event, deadline_count, rem_out);
                }

                if (linux_time_sleep_parse_kmsg(
                            msg, port, KMSG_OP_SYSTEM_TIMER_EXPIRE, token)) {
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                        now = rendezvos_time_now();
                        if (!time_before(now, deadline_count)) {
                                return 0;
                        }
                        continue;
                }

                if (linux_time_sleep_parse_kmsg(
                            msg, port, KMSG_OP_SYSTEM_TIMER_CANCEL, token)) {
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                        return linux_time_sleep_return_eintr(
                                event, deadline_count, rem_out);
                }

                if (linux_ipc_kmsg_is_port_closed(port, msg)) {
                        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                        linux_time_sleep_disarm(event);
                        return linux_time_sleep_return_eintr(
                                event, deadline_count, rem_out);
                }

                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                linux_time_sleep_disarm(event);
        }
}
