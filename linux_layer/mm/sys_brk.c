#include <common/types.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/linux_mm_radix.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

u64 sys_brk(u64 new_brk)
{
        Tcb_Base* tcb = get_cpu_current_task();
        linux_proc_append_t* pa = linux_proc_append(tcb);
        if (!tcb || !pa)
                return 0;

        if (pa->start_brk == 0)
                pa->start_brk = pa->brk;

        if (new_brk == 0)
                return pa->brk;

        u64 old_brk = pa->brk;
        u64 old_aligned = ROUND_UP(old_brk, PAGE_SIZE);
        u64 new_aligned = ROUND_UP(new_brk, PAGE_SIZE);

        /* Grow: map pages. */
        if (new_aligned > old_aligned) {
                u64 page_num = (new_aligned - old_aligned) / PAGE_SIZE;
                ENTRY_FLAGS_t page_flags = PAGE_ENTRY_USER | PAGE_ENTRY_VALID
                                           | PAGE_ENTRY_WRITE | PAGE_ENTRY_READ;
                void* p = linux_mm_map_user_range(tcb->vs,
                                                  (vaddr)old_aligned,
                                                  (size_t)page_num,
                                                  page_flags);
                if (!p) {
                        /* Linux brk: failure returns current brk unchanged. */
                        return pa->brk;
                }
        } else if (new_aligned < old_aligned) {
                /* Shrink: unmap/free pages. */
                u64 page_num = (old_aligned - new_aligned) / PAGE_SIZE;
                (void)linux_mm_unmap_user_range(
                        tcb->vs, (vaddr)new_aligned, (size_t)page_num);
        }

        pa->brk = new_brk;
        return pa->brk;
}
