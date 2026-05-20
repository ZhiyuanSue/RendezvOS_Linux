/*
 * VFS IPC Client Interface
 *
 * Provides request-response mechanism for filesystem syscalls.
 */

#ifndef _LINUX_COMPAT_FS_FS_IPC_H_
#define _LINUX_COMPAT_FS_FS_IPC_H_

#include <common/types.h>

/*
 * Send VFS request and wait for response
 *
 * Args:
 *   opcode: VFS operation opcode (e.g., KMSG_OP_VFS_OPEN)
 *   fmt:    Format string for ipc_serial_encode (e.g., "sippu")
 *   ...:    Variable arguments for encoding
 *
 * Returns:
 *   0 on success, negative error code on failure
 *
 * Usage example:
 *   i64 result = vfs_ipc_request_response(KMSG_OP_VFS_OPEN,
 *                                         "sippu",
 *                                         pathname, flags, mode);
 */
i64 vfs_ipc_request_response(u16 opcode, const char* fmt, ...);

#endif /* _LINUX_COMPAT_FS_FS_IPC_H_ */
