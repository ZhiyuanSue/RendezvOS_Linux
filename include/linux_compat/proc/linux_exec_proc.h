#ifndef _LINUX_COMPAT_PROC_LINUX_EXEC_PROC_H_
#define _LINUX_COMPAT_PROC_LINUX_EXEC_PROC_H_

#include <common/stdbool.h>
#include <common/types.h>
#include <linux_compat/proc_compat.h>
#include <rendezvos/mm/page_slice.h>

/* brk / mmap_hint from PT_LOAD high water (matches exec prepare path).
 */
void linux_proc_set_heap_from_elf_load(linux_proc_resource_t *task, vaddr max_load_end);

/* Clear signal pending state and apply heap layout for a fresh exec image. */
void linux_exec_reset_proc_state(linux_proc_resource_t *task, vaddr max_load_end);

bool linux_exec_elf_slice_valid(struct page_slice *slice);

#endif /* _LINUX_COMPAT_PROC_LINUX_EXEC_PROC_H_ */
