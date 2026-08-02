#include <linux_compat/fs/linux_exec_image.h>

#include <common/string.h>
#include <common/types.h>
#include <linux_compat/debug_trace.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/vfs_exec_load.h>
#include <linux_compat/fs/vfs_kern_load.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/page_slice.h>

/*
 * ELF load for execve / Path B: initramfs (cpio middle layer) then VFS IPC.
 * Embedded _num_app / program_map fallback removed — tests live in cpio.
 */

i64 linux_exec_load_elf_slice(VSpace *vs, const char *filename,
                              struct allocator *alloc,
                              struct page_slice **out_slice)
{
        i64 ret;

        if (!filename || !alloc || !out_slice) {
                return -LINUX_EINVAL;
        }

        *out_slice = NULL;

#if LINUX_COMPAT_TRACE_IPC_WEDGE
        pr_info("[exec] load begin path='%s'\n", filename);
#endif
        ret = vfs_kern_read_file_slice(filename, alloc, out_slice);
        if (ret == 0) {
#if LINUX_COMPAT_TRACE_IPC_WEDGE
                pr_info("[exec] load kern ok path='%s'\n", filename);
#endif
                return 0;
        }
#if LINUX_COMPAT_TRACE_IPC_WEDGE
        pr_info("[exec] load kern ret=%ld path='%s'%s\n",
                (long)ret,
                filename,
                (ret == -LINUX_ENOENT) ? " -> ipc fallback" : "");
#endif
        if (ret != -LINUX_ENOENT) {
                return ret;
        }

        ret = linux_vfs_read_file_for_exec_slice(
                vs, filename, alloc, out_slice);
#if LINUX_COMPAT_TRACE_IPC_WEDGE
        pr_info("[exec] load ipc fallback ret=%ld path='%s'\n",
                (long)ret,
                filename);
#endif
        return ret;
}
