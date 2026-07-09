/*
 * IPC VFS read into page_slice for execve (static ELF64).
 *
 * VFS read targets user VA; map a scratch page, read via IPC, copy into
 * owned kallocator pages bound into the slice.
 */

#include <linux_compat/errno.h>
#include <linux_compat/fs/fs_ipc.h>
#include <linux_compat/fs/vfs_exec_load.h>
#include <linux_compat/fs/vfs_exec_path.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_compat.h>

#include <common/align.h>
#include <common/mm.h>
#include <common/string.h>
#include <common/types.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/page_slice.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>

#define LINUX_AT_FDCWD (-100)
#define LINUX_O_RDONLY 0
#define LINUX_SEEK_SET 0
#define LINUX_SEEK_END 2

#define LINUX_EXEC_MAX_FILE   (8u * 1024u * 1024u)
#define LINUX_EXEC_READ_CHUNK 4096u
#define LINUX_EXEC_SCRATCH_PAGES 1u

static vaddr linux_exec_scratch_hint(VSpace *vs)
{
        linux_proc_append_t *pa = linux_proc_append(get_cpu_current_task());

        (void)vs;
        if (pa && pa->mmap_hint != 0) {
                return (vaddr)ROUND_UP(pa->mmap_hint, PAGE_SIZE);
        }
        if (pa && pa->brk != 0) {
                return (vaddr)ROUND_UP(pa->brk, PAGE_SIZE) + PAGE_SIZE;
        }
        return (vaddr)0x40000000;
}

static vaddr linux_exec_map_scratch(VSpace *vs)
{
        return (vaddr)linux_mm_map_user_range_search(
                vs,
                linux_exec_scratch_hint(vs),
                LINUX_EXEC_SCRATCH_PAGES,
                PAGE_ENTRY_USER | PAGE_ENTRY_VALID | PAGE_ENTRY_READ
                        | PAGE_ENTRY_WRITE,
                32);
}

static i64 linux_vfs_page_slice_err(error_t e)
{
        switch (e) {
        case REND_SUCCESS:
                return 0;
        case -E_IN_PARAM:
                return -LINUX_EINVAL;
        case -E_REND_NO_MEM:
                return -LINUX_ENOMEM;
        default:
                return -LINUX_EIO;
        }
}

static i64 linux_vfs_open_readonly(const char *path, i64 *out_fd)
{
        i64 fd;

        fd = vfs_ipc_request_response(KMSG_OP_VFS_OPEN,
                                      VFS_KMSG_FMT_OPEN,
                                      (i32)LINUX_AT_FDCWD,
                                      path,
                                      (i32)LINUX_O_RDONLY,
                                      (u32)0);
        if (fd < 0) {
                return fd;
        }

        *out_fd = fd;
        return 0;
}

static i64 linux_vfs_query_file_size(i64 fd, i64 *out_size)
{
        i64 file_size;

        file_size = vfs_ipc_request_response(KMSG_OP_VFS_LSEEK,
                                             VFS_KMSG_FMT_LSEEK,
                                             (i32)fd,
                                             (u64)0,
                                             (i32)LINUX_SEEK_END);
        if (file_size < 0) {
                return file_size;
        }
        if (file_size == 0 || (u64)file_size > LINUX_EXEC_MAX_FILE) {
                return -LINUX_EFBIG;
        }

        if (vfs_ipc_request_response(KMSG_OP_VFS_LSEEK,
                                     VFS_KMSG_FMT_LSEEK,
                                     (i32)fd,
                                     (u64)0,
                                     (i32)LINUX_SEEK_SET)
            < 0) {
                return -LINUX_EIO;
        }

        *out_size = file_size;
        return 0;
}

static void linux_vfs_close_quiet(i64 fd)
{
        if (fd >= 0) {
                (void)vfs_ipc_request_response(
                        KMSG_OP_VFS_CLOSE, VFS_KMSG_FMT_CLOSE, (i32)fd);
        }
}

i64 linux_vfs_read_file_slice(VSpace *vs, const char *path,
                              struct allocator *alloc,
                              struct page_slice **out_slice)
{
        i64 fd = -1;
        i64 file_size = 0;
        i64 nread;
        struct page_slice *slice = NULL;
        vaddr scratch = 0;
        u64 copied = 0;
        u64 pgoff = 0;
        vaddr page = 0;
        size_t page_fill = 0;
        i64 ret = -LINUX_EIO;

        if (!vs || !path || !alloc || !out_slice) {
                return -LINUX_EINVAL;
        }

        *out_slice = NULL;

        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        ret = linux_vfs_open_readonly(path, &fd);
        if (ret != 0) {
                return ret;
        }

        ret = linux_vfs_query_file_size(fd, &file_size);
        if (ret != 0) {
                goto out_close;
        }

        slice = page_slice_create(0, (size_t)file_size);
        if (!slice) {
                ret = -LINUX_ENOMEM;
                goto out_close;
        }

        scratch = linux_exec_map_scratch(vs);
        if (!scratch) {
                ret = -LINUX_ENOMEM;
                goto out_destroy_slice;
        }

        while (copied < (u64)file_size) {
                u64 chunk = (u64)file_size - copied;
                error_t e;

                if (chunk > LINUX_EXEC_READ_CHUNK) {
                        chunk = LINUX_EXEC_READ_CHUNK;
                }
                if (chunk > PAGE_SIZE) {
                        chunk = PAGE_SIZE;
                }

                nread = vfs_ipc_request_response(KMSG_OP_VFS_READ,
                                                 VFS_KMSG_FMT_READ,
                                                 (i32)fd,
                                                 scratch,
                                                 chunk);
                if (nread < 0) {
                        ret = nread;
                        goto out_drop_partial_page;
                }
                if (nread == 0) {
                        ret = -LINUX_EIO;
                        goto out_drop_partial_page;
                }

                if (!page) {
                        page = (vaddr)alloc->m_alloc(alloc, PAGE_SIZE);
                        if (!page) {
                                ret = -LINUX_ENOMEM;
                                goto out_unmap;
                        }
                        page_fill = 0;
                }

                e = linux_mm_load_from_user(vs,
                                            scratch,
                                            (void *)(page + page_fill),
                                            (size_t)nread);
                if (e != REND_SUCCESS) {
                        ret = -LINUX_EFAULT;
                        goto out_drop_partial_page;
                }

                page_fill += (size_t)nread;
                copied += (u64)nread;

                if (page_fill < PAGE_SIZE && copied < (u64)file_size) {
                        continue;
                }

                if (page_fill < PAGE_SIZE) {
                        memset((void *)(page + page_fill),
                               0,
                               PAGE_SIZE - page_fill);
                }

                e = page_slice_insert_page(slice, pgoff, page, 0);
                if (e != REND_SUCCESS) {
                        ret = linux_vfs_page_slice_err(e);
                        goto out_drop_partial_page;
                }

                pgoff++;
                page = 0;
                page_fill = 0;
        }

        (void)linux_mm_unmap_user_range(vs, scratch, LINUX_EXEC_SCRATCH_PAGES);
        scratch = 0;

        if (vfs_ipc_request_response(
                    KMSG_OP_VFS_CLOSE, VFS_KMSG_FMT_CLOSE, (i32)fd)
            < 0) {
                page_slice_destroy(&slice);
                return -LINUX_EIO;
        }
        fd = -1;

        *out_slice = slice;
        return 0;

out_drop_partial_page:
        if (page) {
                alloc->m_free(alloc, (void *)page);
        }
out_unmap:
        if (scratch) {
                (void)linux_mm_unmap_user_range(vs, scratch, LINUX_EXEC_SCRATCH_PAGES);
        }
out_destroy_slice:
        if (slice) {
                page_slice_destroy(&slice);
        }
out_close:
        linux_vfs_close_quiet(fd);
        return ret;
}

i64 linux_vfs_read_file_for_exec_slice(VSpace *vs, const char *path,
                                       struct allocator *alloc,
                                       struct page_slice **out_slice)
{
        char alt[256];
        i64 ret;

        ret = linux_vfs_read_file_slice(vs, path, alloc, out_slice);
        if (ret != -LINUX_ENOENT) {
                return ret;
        }

        if (!linux_vfs_exec_path_under_tests(path, alt, sizeof(alt))) {
                return ret;
        }
        if (strcmp(alt, path) == 0) {
                return ret;
        }

        return linux_vfs_read_file_slice(vs, alt, alloc, out_slice);
}
