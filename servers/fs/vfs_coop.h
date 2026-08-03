#ifndef _VFS_COOP_H_
#define _VFS_COOP_H_

#include <linux_compat/ipc/rpc.h>
#include <rendezvos/ipc/kmsg.h>

/*
 * VFS listen coop: per-job cookie (cred + I/O scratch); nested park for
 * READ/WRITE and all path ops including READLINK/MOUNT/GETDENTS. Local
 * one-shots (close/lseek/fstat/umount/backend_register) finish inline.
 */

void vfs_coop_queue_prepare(ipc_rpc_coop_queue_t *q);

ipc_rpc_coop_disp_t vfs_coop_handler(ipc_rpc_coop_job_t *job, u16 opcode,
                                     const kmsg_t *km, i64 *result_out);

#endif /* _VFS_COOP_H_ */
