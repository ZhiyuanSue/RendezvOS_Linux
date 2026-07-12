#ifndef _LINUX_COMPAT_FS_VFS_PROTOCOL_H_
#define _LINUX_COMPAT_FS_VFS_PROTOCOL_H_

#include <common/types.h>

/*
 * VFS-specific opcodes and TLV format strings (transport:
 * linux_compat/ipc/rpc.h). kmsg_hdr.module = vfs_server_port->service_id (see
 * doc/ai/IPC_MESSAGE.md).
 *
 * Scheme B (FD_TABLE.md): OPEN/mkdir/stat use abs path; I/O ops use handle id.
 */

#define VFS_SERVER_PORT_NAME "vfs_server_port"

#define VFS_CLIENT_PORT_PREFIX   "vfs_client_"
#define VFS_CLIENT_PORT_NAME_MAX 32

#define VFS_HANDLE_MAX 128u

/* Bit 31 on OPEN response: target is a directory (compat fd metadata). */
#define VFS_OPEN_RET_IS_DIR_BIT  (1LL << 31)
#define VFS_OPEN_RET_HANDLE_MASK ((1LL << 31) - 1LL)

#define KMSG_OP_VFS_OPEN          1u
#define KMSG_OP_VFS_CLOSE         2u
#define KMSG_OP_VFS_READ          3u
#define KMSG_OP_VFS_WRITE         4u
#define KMSG_OP_VFS_FSTAT         5u
#define KMSG_OP_VFS_STAT          6u
#define KMSG_OP_VFS_LSEEK         7u
#define KMSG_OP_VFS_GETCWD        8u /* deprecated: compat local getcwd */
#define KMSG_OP_VFS_CHDIR         9u
#define KMSG_OP_VFS_DUP3          10u
#define KMSG_OP_VFS_PIPE2         11u
#define KMSG_OP_VFS_MKDIRAT       12u
#define KMSG_OP_VFS_UNLINKAT      13u
#define KMSG_OP_VFS_NEWFSTATAT    14u
#define KMSG_OP_VFS_GETDENTS64    15u
#define KMSG_OP_VFS_HANDLE_RETAIN 16u
#define KMSG_OP_VFS_VALIDATE_DIR  17u
#define KMSG_OP_VFS_MOUNT         18u
#define KMSG_OP_VFS_UMOUNT        19u
#define KMSG_OP_VFS_RENAMEAT      20u
#define KMSG_OP_VFS_LINKAT        21u

/* Response: single i64 (Linux errno or non-negative syscall result). */
#define KMSG_OP_VFS_RESP  0u
#define VFS_KMSG_FMT_RESP "q"

/*
 * Request TLV format strings (without reply port).
 * Client appends reply port name as final TLV type 't' (see fs_ipc.c).
 */
#define VFS_KMSG_FMT_CLOSE         "i"
#define VFS_KMSG_FMT_READ          "ipp"
#define VFS_KMSG_FMT_WRITE         "ipp"
#define VFS_KMSG_FMT_OPEN          "siu"
#define VFS_KMSG_FMT_FSTAT         "ip"
#define VFS_KMSG_FMT_LSEEK         "iqi"
#define VFS_KMSG_FMT_DUP3          "iii"
#define VFS_KMSG_FMT_PIPE2         "pi"
#define VFS_KMSG_FMT_MKDIRAT       "su"
#define VFS_KMSG_FMT_UNLINKAT      "si"
#define VFS_KMSG_FMT_NEWFSTATAT    "spu"
#define VFS_KMSG_FMT_GETDENTS64    "ipp"
#define VFS_KMSG_FMT_CHDIR         "s"
#define VFS_KMSG_FMT_VALIDATE_DIR  "s"
#define VFS_KMSG_FMT_HANDLE_RETAIN "i"
#define VFS_KMSG_FMT_MOUNT         "ssu"
#define VFS_KMSG_FMT_UMOUNT        "su"
#define VFS_KMSG_FMT_RENAMEAT      "ssu"
#define VFS_KMSG_FMT_LINKAT        "ssu"

#endif /* _LINUX_COMPAT_FS_VFS_PROTOCOL_H_ */
