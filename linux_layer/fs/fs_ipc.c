/*
 * VFS IPC client — thin wrapper over linux_compat/ipc/rpc.h
 */

#include <linux_compat/errno.h>
#include <linux_compat/fs/fs_ipc.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/ipc/rpc.h>
#include <modules/log/log.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>

static Message_Port_t* vfs_get_or_create_client_port(void)
{
        Tcb_Base* current = get_cpu_current_task();
        char port_name[VFS_CLIENT_PORT_NAME_MAX];

        if (!current || current->pid <= 0) {
                return NULL;
        }

        if (ipc_rpc_format_port_name(port_name,
                                     sizeof(port_name),
                                     VFS_CLIENT_PORT_PREFIX,
                                     current->pid)
            == 0) {
                return NULL;
        }

        return ipc_rpc_port_lookup_or_create(port_name);
}

i64 vfs_ipc_request_response(u16 opcode, const char* fmt, ...)
{
        Tcb_Base* current = get_cpu_current_task();
        Message_Port_t* vfs_port;
        Message_Port_t* client_port;
        va_list ap;
        i64 ret;

        if (!current) {
                return -LINUX_ESRCH;
        }

        client_port = vfs_get_or_create_client_port();
        if (!client_port) {
                return -LINUX_EIO;
        }

        vfs_port = thread_lookup_port(VFS_SERVER_PORT_NAME);
        if (!vfs_port) {
                ref_put(&client_port->refcount, free_message_port_ref);
                pr_error("[VFS] %s not registered\n", VFS_SERVER_PORT_NAME);
                return -LINUX_ENOSYS;
        }

        va_start(ap, fmt);
        ret = ipc_rpc_call_va(vfs_port,
                              client_port,
                              opcode,
                              fmt,
                              KMSG_OP_VFS_RESP,
                              VFS_KMSG_FMT_RESP,
                              ap);
        va_end(ap);

        ref_put(&vfs_port->refcount, free_message_port_ref);
        ref_put(&client_port->refcount, free_message_port_ref);
        return ret;
}
