#ifndef _LINUX_COMPAT_FS_VFS_PATH_H_
#define _LINUX_COMPAT_FS_VFS_PATH_H_

#include <common/stdbool.h>
#include <common/types.h>

/*
 * Shared VFS path helpers — single implementation in linux_layer/fs/vfs_path.c.
 * Used by servers/fs (namespace, ramfs, mount) and compat (dirfd resolve).
 *
 * All normalized paths use a leading '/' (e.g. "/text.txt", "/mnt").
 */

#define VFS_PATH_MAX 256

void vfs_path_normalize(const char *in, char *out, u64 out_cap);

bool vfs_path_basename(const char *path, char *out, u64 out_cap);

bool vfs_path_direct_child_name(const char *parent, const char *full,
                                char *name_out, u64 name_cap);

bool vfs_path_parent(const char *path, char *out, u64 out_cap);

bool vfs_path_is_root(const char *path);

bool vfs_path_join(const char *base, const char *rel, char *out, u64 out_cap);

#endif /* _LINUX_COMPAT_FS_VFS_PATH_H_ */
