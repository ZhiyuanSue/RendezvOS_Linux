#ifndef _LINUX_COMPAT_FS_LINUX_EXEC_IMAGE_H_
#define _LINUX_COMPAT_FS_LINUX_EXEC_IMAGE_H_

#include <common/types.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/page_slice.h>
#include <rendezvos/mm/vmm.h>

/*
 * Resolve an execve filename to a populated ELF page_slice.
 *
 * Order: middle-layer CPIO slice -> IPC VFS slice (/tests fallback) ->
 * embedded payload table. On success *out_slice is set; caller must
 * page_slice_destroy when done.
 */
i64 linux_exec_load_elf_slice(VSpace *vs, const char *filename,
                              struct allocator *alloc,
                              struct page_slice **out_slice);

#endif /* _LINUX_COMPAT_FS_LINUX_EXEC_IMAGE_H_ */
