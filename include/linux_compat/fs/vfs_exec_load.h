#ifndef _LINUX_COMPAT_FS_VFS_EXEC_LOAD_H_
#define _LINUX_COMPAT_FS_VFS_EXEC_LOAD_H_

#include <common/types.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/page_slice.h>
#include <rendezvos/mm/vmm.h>

/*
 * Read a VFS file into an owned page_slice via IPC (one kallocator page per
 * file pgoff). On success *out_slice holds the image; caller must
 * page_slice_destroy when done.
 */
i64 linux_vfs_read_file_slice(VSpace *vs, const char *path,
                              struct allocator *alloc,
                              struct page_slice **out_slice);

/*
 * Like linux_vfs_read_file_slice, but if @p path is missing tries
 * /tests/<basename> (initramfs layout from pack_user_rootfs.py).
 */
i64 linux_vfs_read_file_for_exec_slice(VSpace *vs, const char *path,
                                       struct allocator *alloc,
                                       struct page_slice **out_slice);

#endif /* _LINUX_COMPAT_FS_VFS_EXEC_LOAD_H_ */
