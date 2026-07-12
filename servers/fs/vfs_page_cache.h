#ifndef _VFS_PAGE_CACHE_H_
#define _VFS_PAGE_CACHE_H_

#include <common/types.h>

#include "vfs_backend_ops.h"

struct page_slice;

void vfs_page_cache_reset(void);
void vfs_page_cache_drop(const char *path);

i64 vfs_page_cache_read_inode(const vfs_inode_t *ino, u64 offset, void *buf,
                              u64 len);

i64 vfs_page_cache_write_inode(const vfs_inode_t *ino, u64 offset,
                               const void *buf, u64 len);

i64 vfs_page_cache_clone_inode(const vfs_inode_t *ino,
                               struct page_slice **out_slice);

/* Write-through mirror after ramfs backend persisted bytes. */
void vfs_page_cache_sync_write(const vfs_inode_t *ino, u64 offset,
                               const void *buf, u64 len);

/* ramfs: copy dirty cache to kmalloc; cpio: drop overlay (no writeback). */
i64 vfs_page_cache_flush_backing(const vfs_inode_t *ino);
void vfs_page_cache_drop_backing(const vfs_inode_t *ino);

#endif /* _VFS_PAGE_CACHE_H_ */
