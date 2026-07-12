#ifndef _VFS_BACKEND_H_
#define _VFS_BACKEND_H_

#include <common/stdbool.h>
#include <common/types.h>

#include "vfs_backend_ops.h"

#include "vfs_kstat.h"

/*
 * Backend I/O — VFS middle layer only knows IPC port names and capability
 * bits returned from LOOKUP. Backends register with vfs_server via IPC;
 * vfs_server maintains the registry used for routing.
 */

#define VFS_BACKEND_PORT_CPIO   "vfs_cpio_backend_port"
#define VFS_BACKEND_PORT_RAMFS  "vfs_ramfs_backend_port"
#define VFS_BACKEND_PORT_BLKDEV "vfs_blkdev_port"

#define VFS_BACKEND_REGISTRY_MAX 8u
#define VFS_BACKEND_FSTYPE_MAX   16u

#define VFS_BACKEND_FSTYPE_CPIO   "cpio"
#define VFS_BACKEND_FSTYPE_RAMFS  "ramfs"
#define VFS_BACKEND_FSTYPE_BLKDEV "blkdev"

/* Registration flags (separate from I/O capability bits). */
#define VFS_BACKEND_REG_ROOT     (1u << 0)
#define VFS_BACKEND_REG_OVERLAY  (1u << 1)

typedef enum vfs_backend_op {
        VFS_BACKEND_OP_LOOKUP = 0,
        VFS_BACKEND_OP_READ,
        VFS_BACKEND_OP_WRITE,
        VFS_BACKEND_OP_TRUNCATE,
        VFS_BACKEND_OP_FLUSH,
        VFS_BACKEND_OP_READDIR,
        VFS_BACKEND_OP_READLINK,
        VFS_BACKEND_OP_MKDIR,
        VFS_BACKEND_OP_CREATE,
        VFS_BACKEND_OP_UNLINK,
        VFS_BACKEND_OP_RENAME,
        VFS_BACKEND_OP_LINK,
} vfs_backend_op_t;

#define VFS_BACKEND_CAP_READ_SOURCE     (1u << 0)
#define VFS_BACKEND_CAP_WRITE_SOURCE    (1u << 1)
#define VFS_BACKEND_CAP_WRITE_CACHE     (1u << 2)
#define VFS_BACKEND_CAP_FLUSH_DROP      (1u << 3)

typedef struct vfs_backend_req {
        const char *port;
        vfs_backend_op_t op;
        const char *path;
        const char *path2;
        vfs_inode_t *ino;
        vfs_inode_t *ino_out;
        u64 offset;
        u64 len;
        void *buf;
        const void *wbuf;
        u64 size_arg;
        u32 mode_arg;
        u64 dir_index;
        vfs_dirent_t *dirent_out;
        char *readlink_buf;
        u64 readlink_cap;
        i64 result;
} vfs_backend_req_t;

typedef i64 (*vfs_backend_service_fn)(vfs_backend_req_t *req);

/*
 * Called from vfs_server RPC handler when a backend registers over IPC.
 * Returns 0 or negative Linux errno.
 */
i64 vfs_backend_register(const char *port_name, const char *fstype, u32 caps,
                         u32 reg_flags);

u32 vfs_backend_caps_for_port(const char *port_name);

const char *vfs_backend_port_for_fstype(const char *fstype);

const char *vfs_backend_root_port(void);

const char *vfs_backend_overlay_port(void);

/*
 * Resolve backend port for metadata writes: longest-prefix mount, else overlay.
 */
const char *vfs_backend_port_for_path(const char *path);

bool vfs_backend_port_known(const char *port_name);

bool vfs_backend_boot_io_ready(void);

void vfs_backend_mark_online(u32 reg_flags);

i64 vfs_backend_dispatch(vfs_backend_req_t *req);

bool vfs_backend_lookup(const char *port, const char *path, vfs_inode_t *out);

i64 vfs_backend_readdir(const char *port, const char *dirpath, u64 index,
                          vfs_dirent_t *out);

i64 vfs_backend_readlink(const char *port, const char *path, char *buf,
                         u64 buf_cap);

i64 vfs_backend_mkdir(const char *port, const char *path, u32 mode);
i64 vfs_backend_create(const char *port, const char *path, u32 mode);
i64 vfs_backend_unlink(const char *port, const char *path);
i64 vfs_backend_rename(const char *port, const char *oldpath,
                       const char *newpath);
i64 vfs_backend_link(const char *port, const char *oldpath, const char *newpath);

#endif /* _VFS_BACKEND_H_ */
