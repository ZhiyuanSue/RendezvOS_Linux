#ifndef _VFS_PAGE_CACHE_H_
#define _VFS_PAGE_CACHE_H_

#include <common/types.h>

void vfs_page_cache_reset(void);

/*
 * Read through bootstrap page cache for read-only cpio files.
 * Returns bytes read (>=0) or Linux errno (<0).
 */
i64 vfs_page_cache_read_cpio(const char *path, const u8 *data, u64 size,
                             u64 offset, void *buf, u64 len);

void vfs_page_cache_drop(const char *path);

#endif /* _VFS_PAGE_CACHE_H_ */
