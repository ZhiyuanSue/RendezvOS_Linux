#ifndef _CPIO_ROFS_H_
#define _CPIO_ROFS_H_

#include <common/stdbool.h>
#include <common/types.h>
#include <linux_compat/fs/vfs_path.h>
#include <rendezvos/error.h>

#include "vfs_kstat.h"

/*
 * Read-only newc cpio backend (initramfs image embedded via .incbin).
 *
 * Upward consumer: vfs_namespace.c, vfs_root.c, vfs_backend_ops.c.
 */

#define CPIO_ROFS_PATH_MAX VFS_PATH_MAX

typedef struct cpio_rofs_stat {
        u32 mode;
        u64 size;
        bool is_dir;
        bool is_symlink;
        const u8 *data;
        u32 nlink;
} cpio_rofs_stat_t;

error_t cpio_rofs_init(const void *image, u64 image_len);
u32 cpio_rofs_parsed_count(void);

bool cpio_rofs_lookup(const char *path, cpio_rofs_stat_t *out);
bool cpio_rofs_ptr_in_image(const void *ptr);
i64 cpio_rofs_read(const cpio_rofs_stat_t *st, u64 offset, void *buf, u64 len);

typedef bool (*cpio_rofs_visit_fn)(const char *path, const cpio_rofs_stat_t *st,
                                   void *ctx);
void cpio_rofs_visit(cpio_rofs_visit_fn fn, void *ctx);

i64 cpio_rofs_readdir(const char *dirpath, u64 index, vfs_dirent_t *out);

#endif /* _CPIO_ROFS_H_ */
