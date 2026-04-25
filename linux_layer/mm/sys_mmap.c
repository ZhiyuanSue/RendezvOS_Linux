#include <common/mm.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <modules/log/log.h>
#include <rendezvos/mm/nexus.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

#include "linux_mm_flags.h"

/*
 * Minimal anonymous mmap(2).
 *
 * Scope (initial):
 * - MAP_ANONYMOUS only, fd must be -1, offset must be 0
 * - tries to honor `addr` as a fixed hint; when addr==0 and MAP_FIXED not set,
 *   uses a simple search from (brk + 1 page) upward.
 *
 * Later:
 * - file-backed mmap, MAP_SHARED, COW, and full VMA semantics.
 */

/* Linux mmap flags subset (x86_64): keep local to linux_layer. */
#define LINUX_MAP_FIXED     0x10
#define LINUX_MAP_ANONYMOUS 0x20

u64 sys_mmap(u64 addr, u64 length, i64 prot, i64 flags, i64 fd, u64 offset)
{
        pr_debug(
                "[mmap] called: addr=%lx, length=%lx, prot=%lx, flags=%lx, fd=%d, offset=%lx\n",
                addr,
                length,
                prot,
                flags,
                fd,
                offset);

        if (length == 0)
                return (u64)(-LINUX_EINVAL);

        if (!(flags & LINUX_MAP_ANONYMOUS))
                return (u64)(-LINUX_ENOSYS);
        if (fd != -1)
                return (u64)(-LINUX_EBADF);
        if (offset != 0)
                return (u64)(-LINUX_EINVAL);

        /* Page-align length. */
        u64 len_aligned = ROUND_UP(length, PAGE_SIZE);
        u64 page_num = len_aligned / PAGE_SIZE;
        if (page_num == 0)
                return (u64)(-LINUX_EINVAL);

        Tcb_Base* tcb = get_cpu_current_task();
        if (!tcb || !tcb->vs || !vs_common_is_table_vspace(tcb->vs))
                return (u64)(-LINUX_ESRCH);

        linux_proc_append_t* pa = linux_proc_append(tcb);
        if (!pa)
                return (u64)(-LINUX_EFAULT);

        ENTRY_FLAGS_t page_flags = linux_prot_to_page_flags(prot);
        pr_debug("[mmap] tcb=%p, pa=%p, brk=%lx, page_flags=%lx\n",
                 tcb,
                 pa,
                 pa->brk,
                 page_flags);

        /* Exact mapping if MAP_FIXED is set. */
        if (flags & LINUX_MAP_FIXED) {
                pr_debug("[mmap] MAP_FIXED: addr=%lx\n", addr);
                if ((addr & (PAGE_SIZE - 1)) != 0)
                        return (u64)(-LINUX_EINVAL);
                void* p = get_free_page((size_t)page_num,
                                        (vaddr)addr,
                                        percpu(nexus_root),
                                        tcb->vs,
                                        page_flags);
                if (!p) {
                        pr_debug("[mmap] MAP_FIXED: get_free_page failed\n");
                        return (u64)(-LINUX_ENOMEM);
                }
                pr_debug("[mmap] MAP_FIXED: success at %p\n", p);
                return (u64)p;
        }

        /*
         * Best-effort search: start from a process-local hint near brk.
         * This is *not* a full Linux VMA allocator; it's a stopgap until we
         * implement proper range search and/or store a dedicated mmap cursor.
         */
        vaddr hint = (vaddr)ROUND_UP(pa->brk, PAGE_SIZE) + PAGE_SIZE;
        if (addr != 0) {
                if ((addr & (PAGE_SIZE - 1)) != 0)
                        return (u64)(-LINUX_EINVAL);
                hint = (vaddr)addr;
        }

        pr_debug("[mmap] searching from hint=%lx, page_num=%lu\n",
                 hint,
                 page_num);

        /* Try a bounded number of probes to avoid infinite loops. */
        const int max_probes = 64;
        for (int i = 0; i < max_probes; i++) {
                void* p = get_free_page((size_t)page_num,
                                        hint,
                                        percpu(nexus_root),
                                        tcb->vs,
                                        page_flags);
                if (p) {
                        pr_debug("[mmap] success at %p (iteration %d)\n", p, i);
                        return (u64)p;
                }
                hint += (vaddr)len_aligned;
        }

        pr_debug("[mmap] failed: no free pages found after %d probes\n",
                 max_probes);
        return (u64)(-LINUX_ENOMEM);
}
