#include <common/mm.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <modules/log/log.h>
#include <linux_compat/linux_mm_radix.h>
#include <rendezvos/mm/map_handler.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

#include "linux_mm_flags.h"

#if defined(_X86_64_)
#include <arch/x86_64/boot/arch_setup.h>
#elif defined(_AARCH64_)
#include <arch/aarch64/boot/arch_setup.h>
#endif

/* Linux mremap flags subset */
#define LINUX_MREMAP_MAYMOVE 0x1

static bool linux_user_va_range_ok(u64 addr, u64 len)
{
        if (len == 0)
                return addr < KERNEL_VIRT_OFFSET;
        if (addr >= KERNEL_VIRT_OFFSET)
                return false;
        if (addr + len < addr || addr + len > KERNEL_VIRT_OFFSET)
                return false;
        return true;
}

static bool linux_mm_range_mapped(VSpace* vs, u64 addr, u64 len_bytes)
{
        if (!vs || len_bytes == 0)
                return false;

        u64 end = addr + len_bytes;
        for (vaddr va = ROUND_DOWN((vaddr)addr, PAGE_SIZE); va < (vaddr)end;
             va += PAGE_SIZE) {
                vaddr rstart;
                ENTRY_FLAGS_t fl;

                if (linux_mm_query_vaddr(vs, va, &rstart, &fl) != REND_SUCCESS)
                        return false;
        }
        return true;
}

static error_t linux_mremap_copy_mapped(VSpace* vs, vaddr src, vaddr dst, u64 len)
{
        struct map_handler* h = &percpu(Map_Handler);
        u64 done = 0;

        while (done < len) {
                vaddr sva = src + (vaddr)done;
                vaddr dva = dst + (vaddr)done;
                vaddr sp = ROUND_DOWN(sva, PAGE_SIZE);
                vaddr dp = ROUND_DOWN(dva, PAGE_SIZE);
                u64 so = (u64)(sva - sp);
                u64 doff = (u64)(dva - dp);
                u64 chunk = PAGE_SIZE - (so > doff ? so : doff);

                if (chunk > len - done)
                        chunk = len - done;

                ENTRY_FLAGS_t sf = 0, df = 0;
                int sl = 0, dl = 0;
                ppn_t sppn = have_mapped(vs, VPN(sp), &sf, &sl, h);
                ppn_t dppn = have_mapped(vs, VPN(dp), &df, &dl, h);

                if (invalid_ppn(sppn) || invalid_ppn(dppn) || sl != 3 || dl != 3)
                        return -E_RENDEZVOS;

                error_t e = map_handler_copy_data_range(
                        h, PADDR(dppn) + doff, PADDR(sppn) + so, chunk);
                if (e != REND_SUCCESS)
                        return e;
                done += chunk;
        }
        return REND_SUCCESS;
}

/*
 * Basic mremap: shrink / same size / grow in place / MAYMOVE grow.
 * Anonymous mappings only; MREMAP_FIXED not supported.
 */
i64 sys_mremap(u64 old_address, u64 old_size, u64 new_size, u64 flags,
               u64 new_address)
{
        (void)new_address;

        pr_debug(
                "[mremap] called: old_address=%lx old_size=%lx new_size=%lx flags=%lx\n",
                old_address,
                old_size,
                new_size,
                flags);

        if (old_size == 0 || new_size == 0)
                return -LINUX_EINVAL;
        if ((old_address & (PAGE_SIZE - 1)) != 0)
                return -LINUX_EINVAL;
        if (flags & ~LINUX_MREMAP_MAYMOVE)
                return -LINUX_EINVAL;

        u64 old_size_aligned = ROUND_UP(old_size, PAGE_SIZE);
        u64 new_size_aligned = ROUND_UP(new_size, PAGE_SIZE);

        if (!linux_user_va_range_ok(old_address, old_size_aligned))
                return -LINUX_EINVAL;

        Tcb_Base* tcb = get_cpu_current_task();
        if (!tcb || !tcb->vs || !linux_vspace_is_user_table(tcb->vs))
                return -LINUX_ESRCH;

        linux_proc_append_t* pa = linux_proc_append(tcb);
        if (!pa)
                return -LINUX_EFAULT;

        if (!linux_mm_range_mapped(tcb->vs, old_address, old_size_aligned))
                return -LINUX_EINVAL;

        if (new_size_aligned < old_size_aligned) {
                u64 unmap_addr = old_address + new_size_aligned;
                u64 unmap_size = old_size_aligned - new_size_aligned;

                error_t e = linux_mm_unmap_user_range(
                        tcb->vs,
                        (vaddr)unmap_addr,
                        (size_t)(unmap_size / PAGE_SIZE));
                if (e != REND_SUCCESS)
                        return -LINUX_EINVAL;

                return (i64)old_address;
        }

        if (new_size_aligned == old_size_aligned)
                return (i64)old_address;

        u64 expand_size = new_size_aligned - old_size_aligned;
        vaddr expand_addr = (vaddr)(old_address + old_size_aligned);

        void* new_pages = linux_mm_map_user_range(
                tcb->vs,
                expand_addr,
                (size_t)(expand_size / PAGE_SIZE),
                PAGE_ENTRY_USER | PAGE_ENTRY_VALID | PAGE_ENTRY_WRITE
                        | PAGE_ENTRY_READ);

        if (new_pages)
                return (i64)old_address;

        if (!(flags & LINUX_MREMAP_MAYMOVE))
                return -LINUX_ENOMEM;

        vaddr search = (vaddr)ROUND_UP(pa->brk, PAGE_SIZE) + (vaddr)PAGE_SIZE;
        void* new_mapping = linux_mm_map_user_range_search(
                tcb->vs,
                search,
                (size_t)(new_size_aligned / PAGE_SIZE),
                PAGE_ENTRY_USER | PAGE_ENTRY_VALID | PAGE_ENTRY_WRITE
                        | PAGE_ENTRY_READ,
                64);

        if (!new_mapping)
                return -LINUX_ENOMEM;

        if (linux_mremap_copy_mapped(
                    tcb->vs,
                    (vaddr)old_address,
                    (vaddr)new_mapping,
                    old_size)
            != REND_SUCCESS) {
                (void)linux_mm_unmap_user_range(
                        tcb->vs,
                        (vaddr)new_mapping,
                        (size_t)(new_size_aligned / PAGE_SIZE));
                return -LINUX_EFAULT;
        }

        (void)linux_mm_unmap_user_range(
                tcb->vs,
                (vaddr)old_address,
                (size_t)(old_size_aligned / PAGE_SIZE));

        return (i64)new_mapping;
}
