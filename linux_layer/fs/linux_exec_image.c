#include <linux_compat/fs/linux_exec_image.h>

#include <common/string.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/vfs_exec_load.h>
#include <linux_compat/fs/vfs_exec_path.h>
#include <linux_compat/fs/vfs_kern_load.h>
#include <linux_compat/mm/linux_page_slice_file.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/page_slice.h>

extern u64 _num_app;

#define EXEC_MAX_PATH 256

static i64 find_embedded_elf_by_name(const char *filename)
{
        static const struct {
                const char *name;
                int index;
        } program_map[] = {
                {"test_echo", 41},
                {"test_execve", 17},
                {"test_execve_simple", 50},
                {"test_signal_delivery", 7},
                {"test_phase2b_signal_basic", 8},
        };
        u64 num_apps = _num_app;
        u32 i;

        if (!filename) {
                return -1;
        }

        for (i = 0; i < sizeof(program_map) / sizeof(program_map[0]); i++) {
                if (strcmp_s(program_map[i].name, filename, EXEC_MAX_PATH)
                    != 0) {
                        continue;
                }
                if (program_map[i].index < (i64)num_apps) {
                        return program_map[i].index;
                }
        }

        return -1;
}

static i64 linux_exec_load_embedded_slice(const char *filename,
                                          struct allocator *alloc,
                                          struct page_slice **out_slice)
{
        vaddr elf_start;
        vaddr elf_end;
        i64 app_index;
        error_t e;

        app_index = find_embedded_elf_by_name(filename);
        if (app_index < 0) {
                app_index = find_embedded_elf_by_name(
                        linux_vfs_exec_path_basename(filename));
        }
        if (app_index < 0) {
                return -LINUX_ENOENT;
        }

        elf_start = *(u64 *)((vaddr)(&_num_app)
                             + (app_index * 2 + 1) * (i64)sizeof(u64));
        elf_end = *(u64 *)((vaddr)(&_num_app)
                           + (app_index * 2 + 2) * (i64)sizeof(u64));
        if (elf_end <= elf_start) {
                return -LINUX_ENOEXEC;
        }

        e = linux_page_slice_copy_from_kva(
                out_slice,
                alloc,
                elf_start,
                (size_t)(elf_end - elf_start));
        if (e != REND_SUCCESS) {
                return -LINUX_ENOMEM;
        }

        return 0;
}

i64 linux_exec_load_elf_slice(VSpace *vs, const char *filename,
                              struct allocator *alloc,
                              struct page_slice **out_slice)
{
        i64 ret;

        if (!filename || !alloc || !out_slice) {
                return -LINUX_EINVAL;
        }

        *out_slice = NULL;

        ret = vfs_kern_read_file_slice(filename, alloc, out_slice);
        if (ret == 0) {
                return 0;
        }
        if (ret != -LINUX_ENOENT) {
                return ret;
        }

        ret = linux_vfs_read_file_for_exec_slice(vs, filename, alloc, out_slice);
        if (ret == 0) {
                return 0;
        }
        if (ret != -LINUX_ENOENT) {
                return ret;
        }

        return linux_exec_load_embedded_slice(filename, alloc, out_slice);
}
