#include <common/mm.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <modules/log/log.h>
#include <rendezvos/mm/nexus.h>
#include <rendezvos/mm/map_handler.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

#include "linux_mm_flags.h"

/* Linux mremap flags subset */
#define LINUX_MREMAP_MAYMOVE  0x1  /* Allow moving the mapping */

/*
 * Basic mremap implementation.
 *
 * Scope (initial):
 * - Supports shrinking and expanding mappings
 * - MREMAP_MAYMOVE for expanding mappings that can't grow in place
 * - Anonymous mappings only
 *
 * Limitations:
 * - Does not support MREMAP_FIXED (new_address hint)
 * - May fail on complex scenarios
 */
i64 sys_mremap(u64 old_address,
              u64 old_size,
              u64 new_size,
              u64 flags,
              u64 new_address)
{
        pr_debug("[mremap] called: old_address=%lx, old_size=%lx, new_size=%lx, flags=%lx, new_address=%lx\n",
                 old_address, old_size, new_size, flags, new_address);

        /* Basic validation */
        if (old_size == 0)
                return -LINUX_EINVAL;
        if ((old_address & (PAGE_SIZE - 1)) != 0)
                return -LINUX_EINVAL;

        /* Page-align sizes */
        u64 old_size_aligned = ROUND_UP(old_size, PAGE_SIZE);
        u64 new_size_aligned = ROUND_UP(new_size, PAGE_SIZE);

        if (new_size == 0)
                return -LINUX_EINVAL;

        Tcb_Base* tcb = get_cpu_current_task();
        if (!tcb || !tcb->vs || !vs_common_is_table_vspace(tcb->vs))
                return -LINUX_ESRCH;

        /* Case 1: Shrinking - unmap the tail */
        if (new_size_aligned < old_size_aligned) {
                u64 unmap_addr = old_address + new_size_aligned;
                u64 unmap_size = old_size_aligned - new_size_aligned;

                pr_debug("[mremap] shrinking: unmapping %lx bytes at %lx\n",
                         unmap_size, unmap_addr);

                error_t e = free_pages((void*)unmap_addr,
                                       unmap_size / PAGE_SIZE,
                                       tcb->vs,
                                       percpu(nexus_root));
                if (e != REND_SUCCESS)
                        return -LINUX_EINVAL;

                return (i64)old_address;
        }

        /* Case 2: Same size - nothing to do */
        if (new_size_aligned == old_size_aligned) {
                pr_debug("[mremap] same size: nothing to do\n");
                return (i64)old_address;
        }

        /* Case 3: Expanding - try to grow in place first */
        u64 expand_size = new_size_aligned - old_size_aligned;
        void* expand_addr = (void*)(old_address + old_size_aligned);

        pr_debug("[mremap] expanding: trying to add %lx bytes at %lx\n",
                 expand_size, (u64)expand_addr);

        /* Try to allocate new pages immediately after the old mapping */
        void* new_pages = get_free_page(expand_size / PAGE_SIZE,
                                       (vaddr)expand_addr,
                                       percpu(nexus_root),
                                       tcb->vs,
                                       PAGE_ENTRY_USER | PAGE_ENTRY_VALID | PAGE_ENTRY_WRITE | PAGE_ENTRY_READ);

        if (new_pages) {
                pr_debug("[mremap] expanded in place to %p\n", new_pages);
                return (i64)old_address;
        }

        /* Case 4: Can't expand in place - check if we can move */
        if (!(flags & LINUX_MREMAP_MAYMOVE)) {
                pr_debug("[mremap] can't expand in place and MAYMOVE not set\n");
                return -LINUX_ENOMEM;
        }

        /* Case 5: Move to new location */
        /* Allocate new mapping */
        void* new_mapping = get_free_page(new_size_aligned / PAGE_SIZE,
                                         0,  /* Let system choose address */
                                         percpu(nexus_root),
                                         tcb->vs,
                                         PAGE_ENTRY_USER | PAGE_ENTRY_VALID | PAGE_ENTRY_WRITE | PAGE_ENTRY_READ);

        if (!new_mapping) {
                pr_debug("[mremap] failed to allocate new mapping\n");
                return -LINUX_ENOMEM;
        }

        pr_debug("[mremap] allocated new mapping at %p, copying data...\n", new_mapping);

        /* Copy data from old mapping to new mapping */
        volatile u8* src = (u8*)old_address;
        volatile u8* dst = (u8*)new_mapping;
        for (u64 i = 0; i < old_size; i++) {
                dst[i] = src[i];
        }

        /* Unmap old mapping */
        error_t e = free_pages((void*)old_address,
                              old_size_aligned / PAGE_SIZE,
                              tcb->vs,
                              percpu(nexus_root));
        if (e != REND_SUCCESS) {
                pr_debug("[mremap] warning: failed to unmap old mapping\n");
                /* Continue anyway, we've already copied the data */
        }

        pr_debug("[mremap] moved mapping from %lx to %p\n", old_address, new_mapping);
        return (i64)new_mapping;
}

