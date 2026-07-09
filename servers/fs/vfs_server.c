/*
 * Global VFS server — dispatches via linux_compat IPC RPC framework.
 *
 * SMP: vfs_root + global port + RPC thread are BSP-once; see initcall.h.
 */

#include <modules/log/log.h>
#include <common/stdbool.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/initcall.h>
#include <linux_compat/ipc/rpc.h>
#include <rendezvos/ipc/ipc_serial.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/thread_loader.h>

#include "vfs_fd.h"
#include "vfs_open.h"
#include "vfs_root.h"
#include "vfs_rpc.h"

extern char rootfs_cpio_start[];
extern char rootfs_cpio_end[];

static Thread_Base* vfs_server_thread_ptr = NULL;
static u16 vfs_server_service_id;
static bool vfs_server_init_done;

static char vfs_server_thread_name[] = "vfs_server_thread";

extern struct Port_Table* global_port_table;

static i64 vfs_rpc_dispatch(pid_t pid, u16 opcode, u64 p1, u64 p2, u64 p3,
                            u64 p4, const char* str)
{
        (void)p4;

        switch (opcode) {
        case KMSG_OP_VFS_OPEN:
                return vfs_openat(pid, (i32)p1, str, (i32)p2, (u32)p3);
        case KMSG_OP_VFS_CLOSE:
                return vfs_fd_close(pid, (i32)p1);
        case KMSG_OP_VFS_READ:
                return vfs_read_fd(pid, (i32)p1, p2, p3);
        case KMSG_OP_VFS_WRITE:
                return vfs_write_fd(pid, (i32)p1, p2, p3);
        case KMSG_OP_VFS_FSTAT:
                return vfs_fstat_fd(pid, (i32)p1, p2);
        case KMSG_OP_VFS_LSEEK:
                return vfs_lseek_fd(pid, (i32)p1, (i64)p2, (i32)p3);
        case KMSG_OP_VFS_NEWFSTATAT:
                return vfs_statat(pid, (i32)p1, str, p2, (i32)p3);
        case KMSG_OP_VFS_MKDIRAT:
                return vfs_mkdirat(pid, (i32)p1, str, (u32)p2);
        case KMSG_OP_VFS_UNLINKAT:
                return vfs_unlinkat(pid, (i32)p1, str, (i32)p2);
        default:
                return -LINUX_ENOSYS;
        }
}

static i64 vfs_rpc_handler(u16 opcode, const kmsg_t* km, char** reply_port_out)
{
        error_t decode_err;
        u64 param1 = 0;
        u64 param2 = 0;
        u64 param3 = 0;
        u64 param4 = 0;
        char* str_param = NULL;
        pid_t pid = 0;

        if (!km || !reply_port_out) {
                return -LINUX_EINVAL;
        }

        *reply_port_out = NULL;
        decode_err = -E_IN_PARAM;

        switch (opcode) {
        case KMSG_OP_VFS_GETCWD:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_GETCWD "t",
                                               &param1,
                                               &param2,
                                               reply_port_out);
                if (decode_err == REND_SUCCESS) {
                        (void)param1;
                        (void)param2;
                        return 0;
                }
                return -LINUX_EINVAL;
        case KMSG_OP_VFS_OPEN:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_OPEN "t",
                                               &param1,
                                               &str_param,
                                               &param2,
                                               &param3,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_CLOSE:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_CLOSE "t",
                                               &param1,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_READ:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_READ "t",
                                               &param1,
                                               &param2,
                                               &param3,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_FSTAT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_FSTAT "t",
                                               &param1,
                                               &param2,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_LSEEK:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_LSEEK "t",
                                               &param1,
                                               &param2,
                                               &param3,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_DUP3:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_DUP3 "t",
                                               &param1,
                                               &param2,
                                               &param3,
                                               reply_port_out);
                return (decode_err == REND_SUCCESS) ? -LINUX_ENOSYS :
                                                      -LINUX_EINVAL;
        case KMSG_OP_VFS_PIPE2:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_PIPE2 "t",
                                               &param1,
                                               &param2,
                                               reply_port_out);
                return (decode_err == REND_SUCCESS) ? -LINUX_ENOSYS :
                                                      -LINUX_EINVAL;
        case KMSG_OP_VFS_MKDIRAT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_MKDIRAT "t",
                                               &param1,
                                               &str_param,
                                               &param2,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_UNLINKAT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_UNLINKAT "t",
                                               &param1,
                                               &str_param,
                                               &param2,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_NEWFSTATAT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_NEWFSTATAT "t",
                                               &param1,
                                               &str_param,
                                               &param2,
                                               &param3,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_WRITE:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_WRITE "t",
                                               &param1,
                                               &param2,
                                               &param3,
                                               reply_port_out);
                break;
        default:
                decode_err = ipc_serial_decode(
                        km->payload, km->hdr.payload_len, "t", reply_port_out);
                return (decode_err == REND_SUCCESS) ? -LINUX_ENOSYS :
                                                      -LINUX_EINVAL;
        }

        if (decode_err != REND_SUCCESS) {
                return -LINUX_EINVAL;
        }

        if (!vfs_rpc_client_pid(*reply_port_out, &pid)) {
                return -LINUX_EINVAL;
        }

        return vfs_rpc_dispatch(
                pid, opcode, param1, param2, param3, param4, str_param);
}

static void vfs_server_thread_entry(void)
{
        ipc_rpc_server_loop(VFS_SERVER_PORT_NAME,
                            vfs_server_service_id,
                            KMSG_OP_VFS_RESP,
                            VFS_KMSG_FMT_RESP,
                            vfs_rpc_handler);
}

static void vfs_server_init(void)
{
        Message_Port_t* port;
        error_t err;
        u64 cpio_len;

        if (!global_port_table) {
                pr_error("[VFS] global_port_table missing on CPU %llu\n",
                         (u64)percpu(cpu_number));
                return;
        }
        if (!linux_init_bsp_once(&vfs_server_init_done)) {
                return;
        }

        pr_info("[VFS] BSP init on CPU %llu\n", (u64)percpu(cpu_number));

        vfs_fd_init();

        cpio_len = (u64)(rootfs_cpio_end - rootfs_cpio_start);
        err = vfs_root_init(rootfs_cpio_start, cpio_len);
        if (err != REND_SUCCESS) {
                pr_error("[VFS] vfs_root_init failed: %d (len=%llu)\n",
                         (int)err,
                         cpio_len);
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

        linux_init_bsp_mark_done(&vfs_server_init_done);
}

DEFINE_INIT(vfs_server_init);
