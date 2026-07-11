/*
 * Install upper-layer handler for core init_thread IPC loop (port is core-owned).
 */

#include <linux_compat/initcall.h>
#include <linux_compat/ipc/exit_protocol.h>
#include <linux_compat/proc/wait_ipc.h>
#include <modules/log/log.h>
#include <rendezvos/ipc/ipc_serial.h>
#include <rendezvos/ipc/kmsg.h>
#include <rendezvos/ipc/message.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/initcall.h>

static bool linux_init_kernel_handler_installed;

static void linux_init_kernel_ipc_handler(Message_t *msg, u16 service_id)
{
        const kmsg_t *km;
        i64 child_pid_i64;
        i32 exit_code;

        if (!msg) {
                return;
        }

        km = kmsg_from_msg(msg);
        if (!km || km->hdr.module != service_id) {
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                return;
        }

        if (km->hdr.opcode != KMSG_OP_PROC_EXIT_NOTIFY) {
                pr_warn("[init_kernel] unknown opcode %u on '%s'\n",
                        (unsigned)km->hdr.opcode,
                        KERNEL_PORT_NAME);
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                return;
        }

        if (ipc_serial_decode(km->payload,
                              km->hdr.payload_len,
                              LINUX_KMSG_FMT_EXIT_NOTIFY,
                              &child_pid_i64,
                              &exit_code)
            != REND_SUCCESS) {
                pr_error("[init_kernel] EXIT_NOTIFY decode failed\n");
                ref_put(&msg->ms_queue_node.refcount, free_message_ref);
                return;
        }

        (void)linux_proc_reap_zombie_by_pid((pid_t)child_pid_i64);
        ref_put(&msg->ms_queue_node.refcount, free_message_ref);
}

static void linux_init_kernel_handler_init(void)
{
        if (!linux_init_bsp_once(&linux_init_kernel_handler_installed)) {
                return;
        }

        kernel_set_ipc_handler(linux_init_kernel_ipc_handler);
        pr_info("[init_kernel] handler installed for '%s'\n",
                KERNEL_PORT_NAME);
        linux_init_bsp_mark_done(&linux_init_kernel_handler_installed);
}

DEFINE_INIT(linux_init_kernel_handler_init);
