#include <common/mm.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <modules/log/log.h>
#include <rendezvos/mm/map_handler.h>
#include <linux_compat/linux_mm_radix.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/thread.h>
#include <syscall.h>

#include "linux_mm_flags.h"

i64 sys_mprotect(u64 addr, u64 length, i64 prot)
{
        if (length == 0)
                return 0;
        if ((addr & (PAGE_SIZE - 1)) != 0)
                return -LINUX_EINVAL;

        u64 len_aligned = ROUND_UP(length, PAGE_SIZE);
        u64 end = addr + len_aligned;
        if (end < addr)
                return -LINUX_EINVAL;

        linux_proc_resource_t* tcb = linux_current_proc();
        if (!tcb || !linux_current_vs() || !linux_vspace_is_user_table(linux_current_vs()))
                return -LINUX_ESRCH;

        ENTRY_FLAGS_t eflags = linux_prot_to_page_flags(prot);
        error_t e = linux_mm_update_range_flags(
                linux_current_vs(), (vaddr)addr, len_aligned, eflags);
        if (e != REND_SUCCESS)
                return -LINUX_EINVAL;
        return 0;
}
