/*
 * Global VFS server — dispatches via linux_compat IPC RPC framework.
 */

#include <modules/log/log.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/ipc/rpc.h>
#include <rendezvos/ipc/ipc_serial.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/thread_loader.h>

static Thread_Base* vfs_server_thread_ptr = NULL;
static u16 vfs_server_service_id;

static char vfs_server_thread_name[] = "vfs_server_thread";

extern cpu_id_t BSP_ID;
extern struct Port_Table* global_port_table;

static i64 vfs_rpc_handler(u16 opcode, const kmsg_t* km, char** reply_port_out)
{
        error_t decode_err;
        u64 param1 = 0;
        u64 param2 = 0;
        u64 param3 = 0;
        char* str_param = NULL;

        if (!km || !reply_port_out) {
                return -LINUX_EINVAL;
        }

        *reply_port_out = NULL;
        decode_err = -E_IN_PARAM;

        switch (opcode) {
        case KMSG_OP_VFS_GETCWD:
                decode_err = ipc_serial_decode(km->payload, km->hdr.payload_len,
                                              VFS_KMSG_FMT_GETCWD "t", &param1,
                                              &param2, reply_port_out);
                if (decode_err == REND_SUCCESS) {
                        (void)param1;
                        (void)param2;
                        return 0;
                }
                return -LINUX_EINVAL;
        case KMSG_OP_VFS_DUP3:
                decode_err = ipc_serial_decode(km->payload, km->hdr.payload_len,
                                              VFS_KMSG_FMT_DUP3 "t", &param1,
                                              &param2, &param3, reply_port_out);
                return (decode_err == REND_SUCCESS) ? -LINUX_ENOSYS
                                                    : -LINUX_EINVAL;
        case KMSG_OP_VFS_PIPE2:
                decode_err = ipc_serial_decode(km->payload, km->hdr.payload_len,
                                              VFS_KMSG_FMT_PIPE2 "t", &param1,
                                              &param2, reply_port_out);
                return (decode_err == REND_SUCCESS) ? -LINUX_ENOSYS
                                                    : -LINUX_EINVAL;
        case KMSG_OP_VFS_MKDIRAT:
                decode_err = ipc_serial_decode(km->payload, km->hdr.payload_len,
                                              VFS_KMSG_FMT_MKDIRAT "t", &param1,
                                              &str_param, &param2, reply_port_out);
                return (decode_err == REND_SUCCESS) ? -LINUX_ENOSYS
                                                    : -LINUX_EINVAL;
        case KMSG_OP_VFS_UNLINKAT:
                decode_err = ipc_serial_decode(km->payload, km->hdr.payload_len,
                                              VFS_KMSG_FMT_UNLINKAT "t", &param1,
                                              &str_param, &param2, reply_port_out);
                return (decode_err == REND_SUCCESS) ? -LINUX_ENOSYS
                                                    : -LINUX_EINVAL;
        case KMSG_OP_VFS_NEWFSTATAT:
                decode_err = ipc_serial_decode(km->payload, km->hdr.payload_len,
                                              VFS_KMSG_FMT_NEWFSTATAT "t", &param1,
                                              &str_param, &param2, &param3,
                                              reply_port_out);
                return (decode_err == REND_SUCCESS) ? -LINUX_ENOSYS
                                                    : -LINUX_EINVAL;
        case KMSG_OP_VFS_CLOSE:
                decode_err = ipc_serial_decode(km->payload, km->hdr.payload_len,
                                              VFS_KMSG_FMT_CLOSE "t", &param1,
                                              reply_port_out);
                return (decode_err == REND_SUCCESS) ? -LINUX_ENOSYS
                                                    : -LINUX_EINVAL;
        case KMSG_OP_VFS_READ:
        case KMSG_OP_VFS_WRITE:
                decode_err = ipc_serial_decode(km->payload, km->hdr.payload_len,
                                              VFS_KMSG_FMT_READ "t", &param1,
                                              &param2, &param3, reply_port_out);
                return (decode_err == REND_SUCCESS) ? -LINUX_ENOSYS
                                                    : -LINUX_EINVAL;
        default:
                decode_err = ipc_serial_decode(km->payload, km->hdr.payload_len,
                                                "t", reply_port_out);
                return (decode_err == REND_SUCCESS) ? -LINUX_ENOSYS
                                                    : -LINUX_EINVAL;
        }
}

static void vfs_server_thread_entry(void)
{
        ipc_rpc_server_loop(VFS_SERVER_PORT_NAME, vfs_server_service_id,
                            KMSG_OP_VFS_RESP, VFS_KMSG_FMT_RESP,
                            vfs_rpc_handler);
}

static void vfs_server_init(void)
{
        cpu_id_t cpu = percpu(cpu_number);
        Message_Port_t* port;
        error_t err;

        if (cpu != BSP_ID || !global_port_table) {
                return;
        }

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

        pr_info("[VFS] registered '%s' service_id=%u\n", VFS_SERVER_PORT_NAME,
                vfs_server_service_id);

        err = gen_thread_from_func(&vfs_server_thread_ptr,
                                   (kthread_func)vfs_server_thread_entry,
                                   vfs_server_thread_name, percpu(core_tm),
                                   NULL);
        if (err != REND_SUCCESS) {
                pr_error("[VFS] gen_thread_from_func failed: %d\n", (int)err);
        }
}

DEFINE_INIT(vfs_server_init);
