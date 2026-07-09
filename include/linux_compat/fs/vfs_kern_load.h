#ifndef _LINUX_COMPAT_FS_VFS_KERN_LOAD_H_
#define _LINUX_COMPAT_FS_VFS_KERN_LOAD_H_

#include <common/types.h>

struct allocator;
struct page_slice;

/*
 * Load a regular file from the VFS middle layer (CPIO initramfs) into an owned
 * page_slice (one kallocator page per pgoff). All middle-layer kernel reads use
 * this path; there is no contiguous whole-file buffer API.
 */
i64 vfs_kern_read_file_slice(const char *path, struct allocator *alloc,
                             struct page_slice **out_slice);

#endif /* _LINUX_COMPAT_FS_VFS_KERN_LOAD_H_ */
