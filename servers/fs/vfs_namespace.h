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

/* Node table: vfs_slice_table. Path rebuilt via parent+name (no path[]). */

typedef struct vfs_ns_node {
        char name[64];
        struct vfs_ns_node *parent;
        struct vfs_ns_node *first_child;
        struct vfs_ns_node *next_sibling;
        u32 mode;
        bool is_dir;
        bool deleted;
        bool in_cpio;
        bool overlay;
        bool is_symlink;
        bool mount_covered;
} vfs_ns_node_t;

error_t vfs_namespace_init(void);

void vfs_namespace_reset(void);

u32 vfs_namespace_count(void);

i64 vfs_namespace_lookup(const char *path, vfs_inode_t *out);

/*
 * Split lookup for coop nested park:
 *   0  — @out filled (no IPC)
 *   1  — need backend LOOKUP (*@port_out + @be_path)
 *  <0  — errno
 */
i64 vfs_namespace_lookup_prepare(const char *path, vfs_inode_t *out,
                                 const char **port_out, char *be_path,
                                 u64 be_path_cap);

/*
 * Mutating path ops: prepare does local checks.
 *   <0 errno
 *    0 unlink catalog-only (no backend)
 *    1 need backend CREATE/MKDIR/UNLINK
 *    2 create only: need LOOKUP of existing file
 * After nested success, call matching commit when *@need_commit.
 */
i64 vfs_namespace_mkdir_prepare(const char *path, u32 mode,
                                const char **port_out, char *norm_out,
                                u64 norm_cap, u32 *mode_out,
                                bool *need_commit);
i64 vfs_namespace_mkdir_commit(const char *norm, u32 mode);

i64 vfs_namespace_create_prepare(const char *path, u32 mode, vfs_inode_t *out,
                                 const char **port_out, char *norm_out,
                                 u64 norm_cap, u32 *mode_out,
                                 bool *need_commit);
i64 vfs_namespace_create_commit(const char *norm, u32 mode, vfs_inode_t *out);

i64 vfs_namespace_unlink_prepare(const char *path, const char **port_out,
                                 char *norm_out, u64 norm_cap,
                                 bool *need_backend, bool *need_commit);
i64 vfs_namespace_unlink_commit(const char *norm);

/*
 * rename/link prepare:
 *   <0 errno
 *    1 need backend RENAME/LINK (*@port_out, norms); then commit
 */
i64 vfs_namespace_rename_prepare(const char *oldpath, const char *newpath,
                                 const char **port_out, char *old_out,
                                 u64 old_cap, char *new_out, u64 new_cap,
                                 bool *need_commit);
i64 vfs_namespace_rename_commit(const char *old_norm, const char *new_norm);

i64 vfs_namespace_link_prepare(const char *oldpath, const char *newpath,
                               const char **port_out, char *old_out,
                               u64 old_cap, char *new_out, u64 new_cap,
                               bool *need_commit);
i64 vfs_namespace_link_commit(const char *old_norm, const char *new_norm);

i64 vfs_namespace_set_mount_cover(const char *target, bool covered);

/*
 * Directory listing prepare for coop:
 *   0 filled locally; 1 EOF; 2 need backend READDIR (*port + be_path);
 *   <0 errno
 */
i64 vfs_namespace_readdir_prepare(const char *dirpath, u64 index,
                                  vfs_dirent_t *out, const char **port_out,
                                  char *be_path, u64 be_path_cap);

#endif /* _VFS_NAMESPACE_H_ */
