#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/task/thread_loader.h>
#include <rendezvos/smp/percpu.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/elf_init.h>

extern u64 _num_app;
#define NR_MAX_TEST NEXUS_PER_PAGE * 3
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

                error_t e =
                        gen_task_from_elf(NULL,
                                          LINUX_PROC_APPEND_BYTES,
                                          LINUX_THREAD_APPEND_BYTES,
                                          app_start,
                                          app_end,
                                          linux_elf_init_handler);
                if (e)
                        continue;
        }
        return REND_SUCCESS;
}
