#include <common/mm.h>
#include <common/types.h>
#include <linux_compat/debug_trace.h>
#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <modules/log/log.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
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

        Tcb_Base* tcb = get_cpu_current_task();
        if (!tcb || !tcb->vs || !linux_vspace_is_user_table(tcb->vs))
                return -LINUX_ESRCH;

#if LINUX_COMPAT_TRACE_MMAP_FS
        pr_info("[mmap-fs] munmap enter addr=0x%llx pages=%d\n",
                (unsigned long long)addr,
                page_num);
#endif
        error_t e = linux_mm_unmap_user_range(
                tcb->vs, (vaddr)addr, (size_t)page_num);
#if LINUX_COMPAT_TRACE_MMAP_FS
        pr_info("[mmap-fs] munmap leave e=%d\n", (int)e);
#endif
        if (e != REND_SUCCESS)
                return -LINUX_EINVAL;
        return 0;
}
