#ifndef _VFS_PATH_H_
#define _VFS_PATH_H_

#include <common/stdbool.h>
#include <common/types.h>

#define VFS_PATH_MAX 256

/*
 * Shared path helpers for cpio_rofs, ramfs_layer, vfs_root.
 * All normalized paths use a leading '/' (e.g. "/text.txt", "/mnt").
 */

void vfs_path_normalize(const char *in, char *out, u64 out_cap);

/* Last path component; returns false if path is "/" or invalid. */
bool vfs_path_basename(const char *path, char *out, u64 out_cap);

/*
 * If @full is a direct child of @parent, write the single path component into
 * @name_out. Both paths may be unnormalized.
 */
bool vfs_path_direct_child_name(const char *parent, const char *full,
                                char *name_out, u64 name_cap);

/*
 * Parent directory. "/a/b" -> "/a"; "/a" -> "/"; "/" -> false.
 */
bool vfs_path_parent(const char *path, char *out, u64 out_cap);

bool vfs_path_is_root(const char *path);

/*
 * Join @base and relative @rel into @out, then normalize.
 * If @rel is absolute, only @rel is normalized into @out.
 */
bool vfs_path_join(const char *base, const char *rel, char *out, u64 out_cap);

#endif /* _VFS_PATH_H_ */
