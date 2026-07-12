#ifndef _VFS_ROOT_H_
#define _VFS_ROOT_H_

#include <common/stdbool.h>
#include <common/types.h>

#include "vfs_backend_ops.h"
#include "vfs_kstat.h"
#include <linux_compat/fs/vfs_path.h>

/*
 * VFS middle layer facade: init + namespace/stat I/O entry points.
 * Namespace logic: vfs_namespace.c (cpio catalog + ramfs storage).
 */

error_t vfs_root_init(const void *cpio_image, u64 cpio_len);

/*
 * Idempotent: safe if vfs_server_init already ran on another CPU.
 * Test runner on BSP may call this before reading initramfs paths.
 */
error_t vfs_root_ensure_init(const void *cpio_image, u64 cpio_len);

/* Linux errno on failure (<0), 0 on success. */
i64 vfs_root_lookup(const char *path, vfs_inode_t *out);

i64 vfs_root_mkdir(const char *path, u32 mode);
i64 vfs_root_create_file(const char *path, u32 mode, vfs_inode_t *out);
i64 vfs_root_unlink(const char *path);

i64 vfs_root_rename(const char *oldpath, const char *newpath);
i64 vfs_root_link(const char *oldpath, const char *newpath);

i64 vfs_root_read(const vfs_inode_t *ino, u64 offset, void *buf, u64 len);
i64 vfs_root_write(vfs_inode_t *ino, u64 offset, const void *buf, u64 len);
i64 vfs_root_truncate(vfs_inode_t *ino, u64 size);

i64 vfs_root_stat(const char *path, vfs_kstat_t *out);

/*
 * Directory listing: stable lexicographic order by name.
 * Returns 0 and fills @out, 1 if @index is past last entry (EOF), <0 errno.
 */
i64 vfs_root_readdir(const char *dirpath, u64 index, vfs_dirent_t *out);

#endif /* _VFS_ROOT_H_ */
