#ifndef _VFS_ROOT_H_
#define _VFS_ROOT_H_

#include <common/types.h>
#include <rendezvos/error.h>

/*
 * Cpio + namespace bring-up. Path lookup/rename/link/readdir:
 * use vfs_namespace_* directly.
 */

error_t vfs_root_init(const void *cpio_image, u64 cpio_len);

#endif /* _VFS_ROOT_H_ */
