#ifndef _VFS_NAMESPACE_H_
#define _VFS_NAMESPACE_H_

#include <common/stdbool.h>
#include <common/types.h>

#include "vfs_backend_ops.h"
#include "vfs_kstat.h"
#include <linux_compat/fs/vfs_path.h>

/*
 * VFS middle layer: tree-structured namespace over cpio (catalog) + ramfs
 * (writable storage). Path existence and delete state live here; backends
 * hold bytes only.
 */

#define VFS_NS_MAX_NODES 256

typedef struct vfs_ns_node {
        char path[VFS_PATH_MAX];
        char name[64];
        struct vfs_ns_node *parent;
        struct vfs_ns_node *first_child;
        struct vfs_ns_node *next_sibling;
        u32 mode;
        bool is_dir;
        bool deleted;
        bool in_cpio;
        bool mount_covered;
        const ramfs_entry_t *ram;
} vfs_ns_node_t;

error_t vfs_namespace_init(void);

void vfs_namespace_reset(void);

u32 vfs_namespace_count(void);

i64 vfs_namespace_lookup(const char *path, vfs_inode_t *out);

i64 vfs_namespace_mkdir(const char *path, u32 mode);
i64 vfs_namespace_create_file(const char *path, u32 mode, vfs_inode_t *out);
i64 vfs_namespace_unlink(const char *path);
i64 vfs_namespace_rename(const char *path, const char *newpath);
i64 vfs_namespace_link(const char *oldpath, const char *newpath);
i64 vfs_namespace_set_mount_cover(const char *target, bool covered);

/*
 * Directory listing: stable lexicographic order by sibling chain.
 * Returns 0 and fills @out, 1 if @index is past last entry (EOF), <0 errno.
 */
i64 vfs_namespace_readdir(const char *dirpath, u64 index, vfs_dirent_t *out);

#endif /* _VFS_NAMESPACE_H_ */
