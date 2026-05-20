#ifndef _LINUX_COMPAT_FS_VFS_PROTOCOL_H_
#define _LINUX_COMPAT_FS_VFS_PROTOCOL_H_

#include <common/types.h>

/*
 * VFS-specific opcodes and TLV format strings (transport: linux_compat/ipc/rpc.h).
 * kmsg_hdr.module = vfs_server_port->service_id (see doc/ai/IPC_MESSAGE.md).
 */

#define VFS_SERVER_PORT_NAME "vfs_server_port"

#define VFS_CLIENT_PORT_PREFIX "vfs_client_"
#define VFS_CLIENT_PORT_NAME_MAX 32

#define KMSG_OP_VFS_OPEN     1u
#define KMSG_OP_VFS_CLOSE    2u
#define KMSG_OP_VFS_READ     3u
#define KMSG_OP_VFS_WRITE    4u
#define KMSG_OP_VFS_FSTAT    5u
#define KMSG_OP_VFS_STAT     6u
#define KMSG_OP_VFS_LSEEK    7u
#define KMSG_OP_VFS_GETCWD   8u
#define KMSG_OP_VFS_CHDIR    9u

/* Response: single i64 (Linux errno or non-negative syscall result). */
#define KMSG_OP_VFS_RESP     0u
#define VFS_KMSG_FMT_RESP    "q"

/*
 * Request TLV format strings (without reply port).
 * Client appends reply port name as final TLV type 't' (see fs_ipc.c).
 */
#define VFS_KMSG_FMT_GETCWD  "pu"
#define VFS_KMSG_FMT_CLOSE   "i"
#define VFS_KMSG_FMT_READ    "ipp"
#define VFS_KMSG_FMT_WRITE   "ipp"
#define VFS_KMSG_FMT_OPEN    "sippu"

#endif /* _LINUX_COMPAT_FS_VFS_PROTOCOL_H_ */
