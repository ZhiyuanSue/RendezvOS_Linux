#include <common/stdbool.h>
#include <common/mm.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/fs_ipc.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/fs/vfs_protocol.h>
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

static vaddr linux_mmap_default_hint(const linux_proc_append_t *pa)
{
        return (vaddr)ROUND_UP(pa->brk, PAGE_SIZE) + PAGE_SIZE;
}

static u64 linux_mmap_map_range(Tcb_Base *tcb, linux_proc_append_t *pa,
                                vaddr hint, u64 len_aligned, u64 page_num,
                                ENTRY_FLAGS_t page_flags, bool fixed)
{
        if (fixed) {
                if ((hint & (PAGE_SIZE - 1)) != 0) {
                        return (u64)(-LINUX_EINVAL);
                }
                void *p = linux_mm_map_user_range(
                        tcb->vs, hint, (size_t)page_num, page_flags);
                if (!p) {
                        return (u64)(-LINUX_ENOMEM);
                }
                u64 end = (u64)p + len_aligned;
                if (pa->mmap_hint < end) {
                        pa->mmap_hint = end;
                }
                return (u64)p;
        }

        const int max_probes = 256;
        for (int i = 0; i < max_probes; i++) {
                if (hint >= USER_SPACE_TOP
                    || (vaddr)(hint + len_aligned) < hint
                    || (vaddr)(hint + len_aligned) > USER_SPACE_TOP) {
                        break;
                }

                if (linux_mm_range_is_free(tcb->vs, hint, (size_t)page_num)) {
                        void *p = linux_mm_map_user_range(
                                tcb->vs, hint, (size_t)page_num, page_flags);
                        if (p) {
                                u64 end = (u64)p + len_aligned;
                                if (pa->mmap_hint < end) {
                                        pa->mmap_hint = end;
                                }
                                return (u64)p;
                        }
                }

                hint += PAGE_SIZE;
        }

        return (u64)(-LINUX_ENOMEM);
}

static vaddr linux_mmap_pick_hint(linux_proc_append_t *pa, u64 addr)
{
        if (addr != 0) {
                return (vaddr)ROUND_DOWN(addr, PAGE_SIZE);
        }
        if (pa->mmap_hint != 0) {
                return (vaddr)ROUND_DOWN(pa->mmap_hint, PAGE_SIZE);
        }
        return linux_mmap_default_hint(pa);
}

static u64 linux_mmap_file(Tcb_Base *tcb, linux_proc_append_t *pa, u64 addr,
                           u64 len_aligned, u64 page_num,
                           ENTRY_FLAGS_t page_flags, i32 fd, u64 offset,
                           i64 flags)
{
        linux_fd_entry_t *ent;
        u64 map_addr;
        i64 file_size;
        i64 read_len;
        i64 n;
        vaddr hint;
        bool fixed = (flags & LINUX_MAP_FIXED) != 0;

        ent = linux_fd_get(tcb, fd);
        if (!ent || ent->kind != LINUX_FD_VFS || ent->is_dir) {
                return (u64)(-LINUX_EBADF);
        }

        if ((offset & (PAGE_SIZE - 1)) != 0) {
                return (u64)(-LINUX_EINVAL);
        }

        file_size = vfs_ipc_request_response(KMSG_OP_VFS_LSEEK,
                                             VFS_KMSG_FMT_LSEEK,
                                             ent->vfs_handle,
                                             0,
                                             2);
        if (file_size < 0) {
                return (u64)file_size;
        }

        if (offset > (u64)file_size) {
                return (u64)(-LINUX_EINVAL);
        }

        hint = fixed ? (vaddr)addr : linux_mmap_pick_hint(pa, addr);
        map_addr = linux_mmap_map_range(tcb,
                                        pa,
                                        hint,
                                        len_aligned,
                                        page_num,
                                        page_flags,
                                        fixed);
        if ((i64)map_addr < 0) {
                return map_addr;
        }

        if ((u64)file_size <= offset) {
                return map_addr;
        }

        read_len = (i64)len_aligned;
        if (offset + (u64)read_len > (u64)file_size) {
                read_len = (i64)((u64)file_size - offset);
        }
        if (read_len <= 0) {
                return map_addr;
        }

        n = vfs_ipc_request_response(KMSG_OP_VFS_LSEEK,
                                     VFS_KMSG_FMT_LSEEK,
                                     ent->vfs_handle,
                                     offset,
                                     0);
        if (n < 0) {
                (void)linux_mm_unmap_user_range(
                        tcb->vs, (vaddr)map_addr, (size_t)page_num);
                return (u64)n;
        }

        n = vfs_ipc_request_response(KMSG_OP_VFS_READ,
                                     VFS_KMSG_FMT_READ,
                                     ent->vfs_handle,
                                     map_addr,
                                     (u64)read_len);
        if (n < 0) {
                (void)linux_mm_unmap_user_range(
                        tcb->vs, (vaddr)map_addr, (size_t)page_num);
                return (u64)n;
        }

        return map_addr;
}

u64 sys_mmap(u64 addr, u64 length, i64 prot, i64 flags, i64 fd, u64 offset)
{
        u64 len_aligned;
        u64 page_num;
        Tcb_Base *tcb;
        linux_proc_append_t *pa;
        ENTRY_FLAGS_t page_flags;
        vaddr hint;
        bool fixed;

        if (length == 0) {
                return (u64)(-LINUX_EINVAL);
        }

        len_aligned = ROUND_UP(length, PAGE_SIZE);
        page_num = len_aligned / PAGE_SIZE;
        if (page_num == 0) {
                return (u64)(-LINUX_EINVAL);
        }

        tcb = get_cpu_current_task();
        if (!tcb || !tcb->vs || !linux_vspace_is_user_table(tcb->vs)) {
                return (u64)(-LINUX_ESRCH);
        }

        pa = linux_proc_append(tcb);
        if (!pa) {
                return (u64)(-LINUX_EFAULT);
        }

        page_flags = linux_prot_to_page_flags(prot);
        fixed = (flags & LINUX_MAP_FIXED) != 0;

        if (!(flags & LINUX_MAP_ANONYMOUS)) {
                if (fd < 0) {
                        return (u64)(-LINUX_EBADF);
                }
                return linux_mmap_file(tcb,
                                       pa,
                                       addr,
                                       len_aligned,
                                       page_num,
                                       page_flags,
                                       (i32)fd,
                                       offset,
                                       flags);
        }

        if (fd != -1) {
                return (u64)(-LINUX_EBADF);
        }
        if (offset != 0) {
                return (u64)(-LINUX_EINVAL);
        }

        if (fixed) {
                if ((addr & (PAGE_SIZE - 1)) != 0) {
                        return (u64)(-LINUX_EINVAL);
                }
                hint = (vaddr)addr;
        } else if (addr != 0) {
                if ((addr & (PAGE_SIZE - 1)) != 0) {
                        return (u64)(-LINUX_EINVAL);
                }
                hint = (vaddr)addr;
        } else {
                hint = linux_mmap_pick_hint(pa, 0);
        }

        return linux_mmap_map_range(
                tcb, pa, hint, len_aligned, page_num, page_flags, fixed);
}
