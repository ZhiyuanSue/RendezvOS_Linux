#ifndef _LINUX_COMPAT_MM_LINUX_PAGE_SLICE_FILE_H_
#define _LINUX_COMPAT_MM_LINUX_PAGE_SLICE_FILE_H_

#include <common/types.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/page_slice.h>

/*
 * Copy a contiguous kernel image into page-aligned slice slots (one owned page
 * per pgoff). Not a core page_slice primitive; linux/servers file paths only.
 */
error_t linux_page_slice_copy_from_kva(struct page_slice** slice_out,
                                       struct allocator* alloc, vaddr src,
                                       size_t size);

/* kva of pgoff 0, or 0 when unmapped. */
vaddr linux_page_slice_file_base(struct page_slice* slice);

#endif /* _LINUX_COMPAT_MM_LINUX_PAGE_SLICE_FILE_H_ */
