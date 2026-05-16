#include <common/mm.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <modules/log/log.h>
#include <linux_compat/linux_mm_radix.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

i64 sys_munmap(u64 addr, u64 length)
{
        pr_debug("[munmap] called: addr=%lx, length=%lx\n", addr, length);

        if (length == 0)
                return -LINUX_EINVAL;
        if ((addr & (PAGE_SIZE - 1)) != 0) {
                pr_debug("[munmap] addr not page-aligned\n");
                return -LINUX_EINVAL;
        }

        u64 len_aligned = ROUND_UP(length, PAGE_SIZE);
        int page_num = (int)(len_aligned / PAGE_SIZE);
        if (page_num <= 0)
                return -LINUX_EINVAL;

        Tcb_Base* tcb = get_cpu_current_task();
        if (!tcb || !tcb->vs || !linux_vspace_is_user_table(tcb->vs))
                return -LINUX_ESRCH;

        pr_debug("[munmap] freeing %d pages at %lx\n", page_num, addr);
        error_t e = linux_mm_unmap_user_range(
                tcb->vs, (vaddr)addr, (size_t)page_num);
        if (e != REND_SUCCESS) {
                pr_debug("[munmap] free_pages failed with error %d\n", e);
                return -LINUX_EINVAL;
        }
        pr_debug("[munmap] success\n");
        return 0;
}
