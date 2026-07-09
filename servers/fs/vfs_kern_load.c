/*
 * Kernel-side VFS file read (middle layer direct; no fd table / IPC).
 */

#include <linux_compat/fs/vfs_kern_load.h>

#include <linux_compat/errno.h>
#include <linux_compat/mm/linux_page_slice_file.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/page_slice.h>

#include "vfs_backend_ops.h"
#include "vfs_root.h"

#define VFS_KERN_MAX_FILE (8u * 1024u * 1024u)

i64 vfs_kern_read_file_slice(const char *path, struct allocator *alloc,
                             struct page_slice **out_slice)
{
        vfs_inode_t ino;
        error_t e;

        if (!path || !out_slice || !alloc) {
                return -LINUX_EINVAL;
        }

        *out_slice = NULL;

        if (vfs_root_lookup(path, &ino) < 0) {
                return -LINUX_ENOENT;
        }
        if (ino.is_dir) {
                return -LINUX_EISDIR;
        }
        if (ino.size <= 0 || ino.size > VFS_KERN_MAX_FILE) {
                return -LINUX_EFBIG;
        }

        if (ino.backend != VFS_BACKEND_CPIO || !ino.u.cpio_data) {
                return -LINUX_ENOENT;
        }

        e = linux_page_slice_copy_from_kva(
                out_slice,
                alloc,
                (vaddr)ino.u.cpio_data,
                (size_t)ino.size);
        return (e == REND_SUCCESS) ? 0 : -LINUX_ENOMEM;
}
