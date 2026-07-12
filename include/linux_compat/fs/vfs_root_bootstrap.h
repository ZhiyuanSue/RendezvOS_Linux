#ifndef _LINUX_COMPAT_FS_VFS_ROOT_BOOTSTRAP_H_
#define _LINUX_COMPAT_FS_VFS_ROOT_BOOTSTRAP_H_

#include <common/types.h>
#include <rendezvos/error.h>

/*
 * Ensure initramfs (cpio) is parsed before kernel-side VFS reads.
 * Idempotent; safe on BSP when vfs_server init ran on another CPU.
 */
error_t linux_vfs_root_ensure_init(void);

/* Block until root + overlay backend threads are registered and serving. */
void linux_vfs_wait_backends_ready(void);

#endif /* _LINUX_COMPAT_FS_VFS_ROOT_BOOTSTRAP_H_ */
