#ifndef _LINUX_COMPAT_PROC_LINUX_EXEC_PROC_H_
#define _LINUX_COMPAT_PROC_LINUX_EXEC_PROC_H_

#include <common/stdbool.h>
#include <common/types.h>
#include <rendezvos/mm/page_slice.h>
#include <rendezvos/task/tcb.h>

/* brk / mmap_hint from PT_LOAD high water (matches linux_elf_init_handler). */
void linux_proc_set_heap_from_elf_load(Tcb_Base *task, vaddr max_load_end);

/* Clear signal pending state and apply heap layout for a fresh exec image. */
void linux_exec_reset_proc_state(Tcb_Base *task, vaddr max_load_end);

bool linux_exec_elf_slice_valid(struct page_slice *slice);

#endif /* _LINUX_COMPAT_PROC_LINUX_EXEC_PROC_H_ */
