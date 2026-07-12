#ifndef _RAMFS_LAYER_H_
#define _RAMFS_LAYER_H_

#include <common/stdbool.h>
#include <common/types.h>
#include <rendezvos/error.h>

#include <linux_compat/fs/vfs_path.h>

/*
 * In-memory writable storage backend (kmalloc buffers).
 * Path existence / delete state: vfs_namespace.c — not here.
 */

#define RAMFS_MAX_ENTRIES   128
#define RAMFS_MAX_FILE_SIZE (256u * 1024u)

#define RAMFS_FLAG_DIR      0x01u

#define RAMFS_S_IFMT  0170000u
#define RAMFS_S_IFDIR 0040000u
#define RAMFS_S_IFREG 0100000u

typedef struct ramfs_entry {
        char path[VFS_PATH_MAX];
        u32 mode;
        u64 size;
        u64 capacity;
        u8 *data;
        u8 flags;
} ramfs_entry_t;

void ramfs_init(void);

u32 ramfs_entry_count(void);

const ramfs_entry_t *ramfs_lookup(const char *path);

error_t ramfs_mkdir(const char *path, u32 mode);
error_t ramfs_create_file(const char *path, u32 mode);
error_t ramfs_unlink(const char *path);
error_t ramfs_rename(const char *oldpath, const char *newpath);
error_t ramfs_link(const char *oldpath, const char *newpath);

i64 ramfs_read(const ramfs_entry_t *ent, u64 offset, void *buf, u64 len);
i64 ramfs_write(ramfs_entry_t *ent, u64 offset, const void *buf, u64 len);
error_t ramfs_truncate(ramfs_entry_t *ent, u64 size);

#endif /* _RAMFS_LAYER_H_ */
