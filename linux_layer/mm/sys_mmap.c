#include <common/mm.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/linux_mm_radix.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

#include "linux_mm_flags.h"

#if defined(_X86_64_)
#include <arch/x86_64/boot/arch_setup.h>
#elif defined(_AARCH64_)
#include <arch/aarch64/boot/arch_setup.h>
#endif

#define LINUX_MAP_FIXED     0x10
#define LINUX_MAP_ANONYMOUS 0x20

static vaddr linux_mmap_default_hint(const linux_proc_append_t* pa)
{
        return (vaddr)ROUND_UP(pa->brk, PAGE_SIZE) + PAGE_SIZE;
}

u64 sys_mmap(u64 addr, u64 length, i64 prot, i64 flags, i64 fd, u64 offset)
{
        if (length == 0)
                return (u64)(-LINUX_EINVAL);

        if (!(flags & LINUX_MAP_ANONYMOUS))
                return (u64)(-LINUX_ENOSYS);
        if (fd != -1)
                return (u64)(-LINUX_EBADF);
        if (offset != 0)
                return (u64)(-LINUX_EINVAL);

        u64 len_aligned = ROUND_UP(length, PAGE_SIZE);
        u64 page_num = len_aligned / PAGE_SIZE;
        if (page_num == 0)
                return (u64)(-LINUX_EINVAL);

        Tcb_Base* tcb = get_cpu_current_task();
        if (!tcb || !tcb->vs || !linux_vspace_is_user_table(tcb->vs))
                return (u64)(-LINUX_ESRCH);

        linux_proc_append_t* pa = linux_proc_append(tcb);
        if (!pa)
                return (u64)(-LINUX_EFAULT);

        ENTRY_FLAGS_t page_flags = linux_prot_to_page_flags(prot);

        if (flags & LINUX_MAP_FIXED) {
                if ((addr & (PAGE_SIZE - 1)) != 0)
                        return (u64)(-LINUX_EINVAL);
                void* p = linux_mm_map_user_range(
                        tcb->vs, (vaddr)addr, (size_t)page_num, page_flags);
                if (!p)
                        return (u64)(-LINUX_ENOMEM);
                u64 end = (u64)p + len_aligned;
                if (pa->mmap_hint < end)
                        pa->mmap_hint = end;
                return (u64)p;
        }

        vaddr hint;
        if (addr != 0) {
                if ((addr & (PAGE_SIZE - 1)) != 0)
                        return (u64)(-LINUX_EINVAL);
                hint = (vaddr)addr;
        } else if (pa->mmap_hint != 0) {
                hint = (vaddr)ROUND_DOWN(pa->mmap_hint, PAGE_SIZE);
        } else {
                hint = linux_mmap_default_hint(pa);
        }

        const int max_probes = 256;
        for (int i = 0; i < max_probes; i++) {
                if (hint >= USER_SPACE_TOP || (vaddr)(hint + len_aligned) < hint
                    || (vaddr)(hint + len_aligned) > USER_SPACE_TOP) {
                        break;
                }

                if (linux_mm_range_is_free(tcb->vs, hint, (size_t)page_num)) {
                        void* p = linux_mm_map_user_range(
                                tcb->vs, hint, (size_t)page_num, page_flags);
                        if (p) {
                                u64 end = (u64)p + len_aligned;
                                if (pa->mmap_hint < end)
                                        pa->mmap_hint = end;
                                return (u64)p;
                        }
                }

                hint += PAGE_SIZE;
        }

        return (u64)(-LINUX_ENOMEM);
}
