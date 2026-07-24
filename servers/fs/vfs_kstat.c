/*
 * kstat / dirent helpers for the VFS middle layer.
 */

#include "vfs_kstat.h"

#include <common/string.h>

u64 vfs_path_to_ino(const char *path)
{
        const u64 offset = 14695981039346656037ULL;
        const u64 prime = 1099511628211ULL;
        u64 h = offset;
        const char *p = path;

        if (!p) {
                return 1;
        }

        while (*p) {
                h ^= (u64)(u8)(*p++);
                h *= prime;
        }

        if (h == 0) {
                h = 1;
        }
        return h;
}

void vfs_kstat_from_inode(const vfs_inode_t *ino, vfs_kstat_t *out)
{
        if (!ino || !out) {
                return;
        }

        memset(out, 0, sizeof(*out));
        out->st_dev = 1;
        out->st_ino = vfs_path_to_ino(ino->path);
        out->st_mode = ino->mode;
        out->st_nlink = ino->nlink ? ino->nlink : 1;
        out->st_uid = 0;
        out->st_gid = 0;
        out->st_size = (i64)ino->size;
        out->st_blksize = 4096;
        out->st_blocks = (u64)((ino->size + 511) / 512);
}

void linux_user_stat_from_kstat(const vfs_kstat_t *in, linux_user_stat_t *out)
{
        if (!in || !out) {
                return;
        }

        memset(out, 0, sizeof(*out));
        out->st_dev = in->st_dev;
        out->st_ino = in->st_ino;
        out->st_mode = in->st_mode;
        out->st_nlink = in->st_nlink;
        out->st_uid = in->st_uid;
        out->st_gid = in->st_gid;
        out->st_rdev = in->st_rdev;
        out->st_size = in->st_size;
        out->st_blksize = in->st_blksize;
        out->st_blocks = in->st_blocks;
        out->st_atime_sec = in->st_atime_sec;
        out->st_atime_nsec = in->st_atime_nsec;
        out->st_mtime_sec = in->st_mtime_sec;
        out->st_mtime_nsec = in->st_mtime_nsec;
        out->st_ctime_sec = in->st_ctime_sec;
        out->st_ctime_nsec = in->st_ctime_nsec;
}
