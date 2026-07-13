/*
 * Global VFS server — dispatches via linux_compat IPC RPC framework.
 */

#include <modules/log/log.h>
#include <common/stdbool.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/initcall.h>
#include <rendezvos/task/initcall.h>
#include <linux_compat/ipc/rpc.h>
#include <rendezvos/ipc/ipc_serial.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/thread_loader.h>

#include "vfs_handle.h"
#include "vfs_mount.h"
#include "vfs_open.h"
#include "vfs_perm.h"
#include "vfs_root.h"
#include "vfs_rpc.h"
#include "vfs_backend.h"

extern char rootfs_cpio_start[];
extern char rootfs_cpio_end[];

static Thread_Base *vfs_server_thread_ptr = NULL;
static u16 vfs_server_service_id;
static bool vfs_service_data_done;
static bool vfs_server_thread_done;

static char vfs_server_thread_name[] = "vfs_server_thread";

extern struct Port_Table *global_port_table;

static i64 vfs_rpc_dispatch(pid_t pid, u16 opcode, u64 p1, u64 p2, u64 p3,
                            u64 p4, const char *str, const char *str2)
{
        (void)p4;

        switch (opcode) {
        case KMSG_OP_VFS_OPEN:
                return vfs_open_path(str, (i32)p1, (u32)p2);
        case KMSG_OP_VFS_CLOSE:
                return vfs_handle_close((u32)p1);
        case KMSG_OP_VFS_HANDLE_RETAIN:
                return vfs_handle_retain((u32)p1);
        case KMSG_OP_VFS_READ:
                return vfs_read_handle(pid, (u32)p1, p2, p3);
        case KMSG_OP_VFS_WRITE:
                return vfs_write_handle(pid, (u32)p1, p2, p3);
        case KMSG_OP_VFS_FSTAT:
                return vfs_fstat_handle(pid, (u32)p1, p2);
        case KMSG_OP_VFS_LSEEK:
                return vfs_lseek_handle((u32)p1, (i64)p2, (i32)p3);
        case KMSG_OP_VFS_NEWFSTATAT:
                return vfs_stat_path(pid, str, p1, (i32)p2);
        case KMSG_OP_VFS_MKDIRAT:
                return vfs_mkdir_path(str, (u32)p1);
        case KMSG_OP_VFS_UNLINKAT:
                return vfs_unlink_path(str, (i32)p1);
        case KMSG_OP_VFS_CHDIR:
                return vfs_validate_dir(str);
        case KMSG_OP_VFS_VALIDATE_DIR:
                return vfs_validate_dir(str);
        case KMSG_OP_VFS_MOUNT:
                return vfs_mount_register(str, str2, p2);
        case KMSG_OP_VFS_UMOUNT:
                return vfs_mount_unregister(str, p2);
        case KMSG_OP_VFS_RENAMEAT:
                return vfs_rename_path(str, str2, (i32)p2);
        case KMSG_OP_VFS_LINKAT:
                return vfs_link_path(str, str2, (i32)p2);
        case KMSG_OP_VFS_GETDENTS64:
                return vfs_getdents64_handle(pid, (u32)p1, p2, p3);
        case KMSG_OP_VFS_READLINKAT:
                return vfs_readlink_path(pid, str, p2, p3);
        case KMSG_OP_VFS_FACCESSAT:
                return vfs_faccessat_path(pid, str, (u32)p1, (u32)p2);
        default:
                return -LINUX_ENOSYS;
        }
}

static i64 vfs_rpc_handler(u16 opcode, const kmsg_t *km, char **reply_port_out)
{
        error_t decode_err;
        u64 param1 = 0;
        u64 param2 = 0;
        u64 param3 = 0;
        u64 param4 = 0;
        char *str_param = NULL;
        char *str_param2 = NULL;
        pid_t pid = 0;

        if (!km || !reply_port_out) {
                return -LINUX_EINVAL;
        }

        *reply_port_out = NULL;
        decode_err = -E_IN_PARAM;

        switch (opcode) {
        case KMSG_OP_VFS_OPEN:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_OPEN "t",
                                               &str_param,
                                               &param1,
                                               &param2,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_CLOSE:
        case KMSG_OP_VFS_HANDLE_RETAIN:
                decode_err = ipc_serial_decode(
                        km->payload,
                        km->hdr.payload_len,
                        (opcode == KMSG_OP_VFS_CLOSE) ?
                                VFS_KMSG_FMT_CLOSE "t" :
                                VFS_KMSG_FMT_HANDLE_RETAIN "t",
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
                                               &str_param,
                                               &param1,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_UNLINKAT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_UNLINKAT "t",
                                               &str_param,
                                               &param1,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_NEWFSTATAT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_NEWFSTATAT "t",
                                               &str_param,
                                               &param1,
                                               &param2,
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
        case KMSG_OP_VFS_GETDENTS64:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_GETDENTS64 "t",
                                               &param1,
                                               &param2,
                                               &param3,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_CHDIR:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_CHDIR "t",
                                               &str_param,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_VALIDATE_DIR:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_VALIDATE_DIR "t",
                                               &str_param,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_MOUNT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_MOUNT "t",
                                               &str_param,
                                               &str_param2,
                                               &param2,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_UMOUNT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_UMOUNT "t",
                                               &str_param,
                                               &param2,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_RENAMEAT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_RENAMEAT "t",
                                               &str_param,
                                               &str_param2,
                                               &param2,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_LINKAT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_LINKAT "t",
                                               &str_param,
                                               &str_param2,
                                               &param2,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_BACKEND_REGISTER: {
                char *port_name = NULL;
                char *fstype = NULL;

                decode_err = ipc_serial_decode(
                        km->payload,
                        km->hdr.payload_len,
                        VFS_KMSG_FMT_BACKEND_REGISTER "t",
                        &port_name,
                        &fstype,
                        &param1,
                        &param2,
                        reply_port_out);
                if (decode_err != REND_SUCCESS) {
                        return -LINUX_EINVAL;
                }
                return vfs_backend_register(port_name,
                                            fstype,
                                            (u32)param1,
                                            (u32)param2);
        }
        case KMSG_OP_VFS_GETCWD:
                return -LINUX_ENOSYS;
        case KMSG_OP_VFS_READLINKAT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_READLINKAT "t",
                                               &str_param,
                                               &param2,
                                               &param3,
                                               reply_port_out);
                break;
        case KMSG_OP_VFS_FACCESSAT:
                decode_err = ipc_serial_decode(km->payload,
                                               km->hdr.payload_len,
                                               VFS_KMSG_FMT_FACCESSAT "t",
                                               &str_param,
                                               &param1,
                                               &param2,
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

        {
                u32 uid = 0;
                u32 gid = 0;

                if (vfs_perm_task_cred(pid, &uid, &gid)) {
                        vfs_perm_set_request(uid, gid);
                } else {
                        vfs_perm_set_request(0, 0);
                }
        }

        return vfs_rpc_dispatch(
                pid, opcode, param1, param2, param3, param4, str_param,
                str_param2);
}

static void vfs_server_thread_entry(void)
{
        ipc_rpc_server_loop(VFS_SERVER_PORT_NAME,
                            vfs_server_service_id,
                            KMSG_OP_VFS_RESP,
                            VFS_KMSG_FMT_RESP,
                            vfs_rpc_handler);
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

        vfs_handle_init();

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
