#ifndef _VFS_KSTAT_H_
#define _VFS_KSTAT_H_

#include <common/types.h>

#include "vfs_backend_ops.h"

/*
 * Linux struct kstat layout used by oscomp user tests (see user_payload
 * stddef.h). Middle layer fills this; front-end copies to user memory.
 */

typedef struct vfs_kstat {
        u64 st_dev;
        u64 st_ino;
        u32 st_mode;
        u32 st_nlink;
        u32 st_uid;
        u32 st_gid;
        u64 st_rdev;
        unsigned long __pad;
        i64 st_size;
        u32 st_blksize;
        i32 __pad2;
        u64 st_blocks;
        i64 st_atime_sec;
        i64 st_atime_nsec;
        i64 st_mtime_sec;
        i64 st_mtime_nsec;
        i64 st_ctime_sec;
        i64 st_ctime_nsec;
} vfs_kstat_t;

/* Linux d_type values for getdents64 (future front-end). */
#define VFS_DT_UNKNOWN 0
#define VFS_DT_DIR     4
#define VFS_DT_REG     8

typedef struct vfs_dirent {
        char name[VFS_PATH_MAX];
        u8 d_type;
        u64 d_ino;
} vfs_dirent_t;

void vfs_kstat_from_inode(const vfs_inode_t *ino, vfs_kstat_t *out);
u64 vfs_path_to_ino(const char *path);

#endif /* _VFS_KSTAT_H_ */
