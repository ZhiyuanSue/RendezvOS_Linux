#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/page_slice.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/task/thread_loader.h>
#include <rendezvos/smp/percpu.h>
#include <linux_compat/elf_init.h>
#include <linux_compat/mm/linux_page_slice_file.h>
#include <linux_compat/proc_compat.h>

extern u64 _num_app;
/* User payload slot table size (was NEXUS_PER_PAGE * 3; 512 slots per 4KiB
 * radix row). */
#define NR_MAX_TEST (512u * 3u)
extern void* test_ptrs[NR_MAX_TEST];
int task_test(void)
{
        pr_info("%lx apps\n", _num_app);
        u64* app_start_ptr;
        u64* app_end_ptr;
        for (u64 i = 0; i < _num_app; i++) {
                app_start_ptr =
                        (u64*)((vaddr)(&_num_app) + (i * 2 + 1) * sizeof(u64));
                app_end_ptr =
                        (u64*)((vaddr)(&_num_app) + (i * 2 + 2) * sizeof(u64));
                u64 app_start = *(app_start_ptr);
                u64 app_end = *(app_end_ptr);
                struct page_slice *slice = NULL;
                struct allocator *alloc = percpu(kallocator);
                error_t e;

                if (!alloc || app_end <= app_start) {
                        continue;
                }

                e = linux_page_slice_copy_from_kva(
                        &slice,
                        alloc,
                        (vaddr)app_start,
                        (size_t)(app_end - app_start));
                if (e != REND_SUCCESS || !slice) {
                        continue;
                }

                e = gen_task_from_elf(NULL,
                                      LINUX_PROC_APPEND_BYTES,
                                      LINUX_THREAD_APPEND_BYTES,
                                      slice,
                                      linux_elf_init_handler_ptr);
                if (e != REND_SUCCESS) {
                        page_slice_destroy(&slice);
                        continue;
                }
        }
        return REND_SUCCESS;
}
