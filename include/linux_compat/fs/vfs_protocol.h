#ifndef _LINUX_COMPAT_FS_VFS_PROTOCOL_H_
#define _LINUX_COMPAT_FS_VFS_PROTOCOL_H_

#include <common/types.h>
#include <rendezvos/ipc/kmsg_system.h>

/*
 * VFS-specific opcodes and TLV format strings (transport:
 * linux_compat/ipc/rpc.h). kmsg_hdr.module = vfs_server_port->service_id (see
 * doc/ai/IPC_MESSAGE.md).
 *
 * Scheme B (FD_TABLE.md): OPEN/mkdir/stat use abs path; I/O ops use handle id.
 *
 * Opcodes start above core system kmsg range (see kmsg_system.h).
 */

#define VFS_SERVER_PORT_NAME "vfs_server_port"

#define VFS_CLIENT_PORT_PREFIX   "vfs_client_"
#define VFS_CLIENT_PORT_NAME_MAX 32

#define VFS_HANDLE_MAX 128u

/* Bit 31 on OPEN response: target is a directory (compat fd metadata). */
#define VFS_OPEN_RET_IS_DIR_BIT  (1LL << 31)
#define VFS_OPEN_RET_HANDLE_MASK ((1LL << 31) - 1LL)

#define KMSG_OP_VFS_FIRST (KMSG_OP_SYSTEM_END + 1u)

#define KMSG_OP_VFS_OPEN               (KMSG_OP_VFS_FIRST + 0u)
#define KMSG_OP_VFS_CLOSE              (KMSG_OP_VFS_FIRST + 1u)
#define KMSG_OP_VFS_READ               (KMSG_OP_VFS_FIRST + 2u)
#define KMSG_OP_VFS_WRITE              (KMSG_OP_VFS_FIRST + 3u)
#define KMSG_OP_VFS_FSTAT              (KMSG_OP_VFS_FIRST + 4u)
#define KMSG_OP_VFS_STAT               (KMSG_OP_VFS_FIRST + 5u)
#define KMSG_OP_VFS_LSEEK              (KMSG_OP_VFS_FIRST + 6u)
#define KMSG_OP_VFS_GETCWD             (KMSG_OP_VFS_FIRST + 7u) /* deprecated */
#define KMSG_OP_VFS_CHDIR              (KMSG_OP_VFS_FIRST + 8u)
#define KMSG_OP_VFS_DUP3               (KMSG_OP_VFS_FIRST + 9u)
#define KMSG_OP_VFS_PIPE2              (KMSG_OP_VFS_FIRST + 10u)
#define KMSG_OP_VFS_MKDIRAT            (KMSG_OP_VFS_FIRST + 11u)
#define KMSG_OP_VFS_UNLINKAT           (KMSG_OP_VFS_FIRST + 12u)
#define KMSG_OP_VFS_NEWFSTATAT         (KMSG_OP_VFS_FIRST + 13u)
#define KMSG_OP_VFS_GETDENTS64         (KMSG_OP_VFS_FIRST + 14u)
#define KMSG_OP_VFS_HANDLE_RETAIN      (KMSG_OP_VFS_FIRST + 15u)
#define KMSG_OP_VFS_VALIDATE_DIR       (KMSG_OP_VFS_FIRST + 16u)
#define KMSG_OP_VFS_MOUNT              (KMSG_OP_VFS_FIRST + 17u)
#define KMSG_OP_VFS_UMOUNT             (KMSG_OP_VFS_FIRST + 18u)
#define KMSG_OP_VFS_RENAMEAT           (KMSG_OP_VFS_FIRST + 19u)
#define KMSG_OP_VFS_LINKAT             (KMSG_OP_VFS_FIRST + 20u)
#define KMSG_OP_VFS_BACKEND_REGISTER   (KMSG_OP_VFS_FIRST + 21u)
#define KMSG_OP_VFS_READLINKAT         (KMSG_OP_VFS_FIRST + 22u)
#define KMSG_OP_VFS_FACCESSAT          (KMSG_OP_VFS_FIRST + 23u)

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
#define VFS_KMSG_FMT_BACKEND_REGISTER "ssuu"
#define VFS_KMSG_FMT_READLINKAT       "spu"
#define VFS_KMSG_FMT_FACCESSAT        "suu"

#endif /* _LINUX_COMPAT_FS_VFS_PROTOCOL_H_ */
