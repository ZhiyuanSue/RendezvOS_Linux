/*
 * Global VFS server — coop listen loop (decode in vfs_coop.c).
 */

#include <modules/log/log.h>
#include <common/stdbool.h>
#include <common/types.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/initcall.h>
#include <linux_compat/ipc/rpc.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/task/thread.h>
#include <rendezvos/task/thread_loader.h>

#include "vfs_coop.h"
#include "vfs_handle.h"
#include "vfs_root.h"

extern char rootfs_cpio_start[];
extern char rootfs_cpio_end[];

static Thread_Base *vfs_server_thread_ptr = NULL;
static u16 vfs_server_service_id;
static bool vfs_service_data_done;
static bool vfs_server_thread_done;

static char vfs_server_thread_name[] = "vfs_server_thread";

extern struct Port_Table *global_port_table;

static ipc_rpc_coop_queue_t vfs_server_coop_q;

Thread_Base *vfs_server_thread_get(void)
{
        return vfs_server_thread_ptr;
}

static void vfs_server_thread_entry(void)
{
        vfs_coop_queue_prepare(&vfs_server_coop_q);
        ipc_rpc_coop_server_loop(VFS_SERVER_PORT_NAME,
                                 vfs_server_service_id,
                                 KMSG_OP_VFS_RESP,
                                 VFS_KMSG_FMT_RESP,
                                 vfs_coop_handler,
                                 &vfs_server_coop_q,
                                 NULL,
                                 NULL);
}

static void vfs_service_data_init(void)
{
        error_t err;
        u64 cpio_len;

        if (!global_port_table) {
                pr_error("[VFS] global_port_table missing on CPU %llu\n",
                         (u64)percpu(cpu_number));
                return;
        }
        if (!linux_init_bsp_once(&vfs_service_data_done)) {
                return;
        }

        pr_info("[VFS] data init on BSP CPU %llu\n", (u64)percpu(cpu_number));

        err = vfs_handle_init();
        if (err != REND_SUCCESS) {
                pr_error("[VFS] vfs_handle_init failed: %d\n", (int)err);
                return;
        }

        cpio_len = (u64)(rootfs_cpio_end - rootfs_cpio_start);
        err = vfs_root_init(rootfs_cpio_start, cpio_len);
        if (err != REND_SUCCESS) {
                pr_error("[VFS] vfs_root_init failed: %d (len=%llu)\n",
                         (int)err,
                         cpio_len);
                return;
        }

        linux_init_bsp_mark_done(&vfs_service_data_done);
}

DEFINE_INIT_LEVEL(vfs_service_data_init, 3);

static void vfs_server_init(void)
{
        Message_Port_t *port;
        error_t err;

        if (!global_port_table) {
                pr_error("[VFS] global_port_table missing on CPU %llu\n",
                         (u64)percpu(cpu_number));
                return;
        }
        if (!linux_init_vfs_service_once(&vfs_server_thread_done)) {
                return;
        }

        pr_info("[VFS] server thread init on CPU %llu\n",
                (u64)percpu(cpu_number));

        port = create_message_port(VFS_SERVER_PORT_NAME);
        if (!port) {
                pr_error("[VFS] create_message_port failed\n");
                return;
        }

        vfs_server_service_id = port->service_id;

        err = register_port(global_port_table, port);
        if (err != REND_SUCCESS) {
                delete_message_port_structure(port);
                pr_error("[VFS] register_port failed: %d\n", (int)err);
                return;
        }

        pr_info("[VFS] registered '%s' service_id=%u on CPU %llu\n",
                VFS_SERVER_PORT_NAME,
                vfs_server_service_id,
                (u64)percpu(cpu_number));

        err = gen_thread_from_func(&vfs_server_thread_ptr,
                                   (kthread_func)vfs_server_thread_entry,
                                   vfs_server_thread_name,
                                   percpu(core_tm),
                                   NULL);
        if (err != REND_SUCCESS) {
                pr_error("[VFS] gen_thread_from_func failed: %d\n", (int)err);
                return;
        }

        linux_init_vfs_service_mark_done(&vfs_server_thread_done);
}

DEFINE_INIT_LEVEL(vfs_server_init, 4);
