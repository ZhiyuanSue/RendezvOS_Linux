/*
 * Kernel-side VFS file read (middle layer direct; no fd table / IPC).
 * Uses page cache + page_slice clone (same path as user read fill).
 */

#include <linux_compat/fs/vfs_kern_load.h>

#include <linux_compat/errno.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/page_slice.h>

#include "vfs_backend_ops.h"
#include "vfs_page_cache.h"
#include "vfs_root.h"

#define VFS_KERN_MAX_FILE (8u * 1024u * 1024u)

i64 vfs_kern_read_file_slice(const char *path, struct allocator *alloc,
                             struct page_slice **out_slice)
{
        vfs_inode_t ino;

        (void)alloc;

        if (!path || !out_slice) {
                return -LINUX_EINVAL;
        }

        *out_slice = NULL;

        {
                i64 lookup_ret = vfs_root_lookup(path, &ino);

                if (lookup_ret < 0) {
                        return lookup_ret;
                }
        }
        if (ino.is_dir) {
                return -LINUX_EISDIR;
        }
        if (ino.size <= 0 || ino.size > VFS_KERN_MAX_FILE) {
                return -LINUX_EFBIG;
        }

        return vfs_page_cache_clone_inode(&ino, out_slice);
}
