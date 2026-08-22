#include <common/mm.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_compat.h>
#include <modules/log/log.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/thread.h>
#include <syscall.h>

i64 sys_munmap(u64 addr, u64 length)
{
        if (length == 0)
                return -LINUX_EINVAL;
        if ((addr & (PAGE_SIZE - 1)) != 0)
                return -LINUX_EINVAL;

        u64 len_aligned = ROUND_UP(length, PAGE_SIZE);
        int page_num = (int)(len_aligned / PAGE_SIZE);
        if (page_num <= 0)
                return -LINUX_EINVAL;

        linux_proc_resource_t* tcb = linux_current_proc();
        if (!tcb || !linux_current_vs() || !linux_vspace_is_user_table(linux_current_vs()))
                return -LINUX_ESRCH;

        error_t e = linux_mm_unmap_user_range(
                linux_current_vs(), (vaddr)addr, (size_t)page_num);
        if (e != REND_SUCCESS)
                return -LINUX_EINVAL;
        return 0;
}
