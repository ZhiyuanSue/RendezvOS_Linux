#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/task/thread_loader.h>
#include <rendezvos/smp/percpu.h>

extern u64 _num_app;

int task_test(void)
{
        if (percpu(cpu_number) == 0) {
                pr_info("%x apps\n", _num_app);
                u64* app_start_ptr;
                u64* app_end_ptr;
                for (u64 i = 0; i < _num_app; i++) {
                        app_start_ptr = (u64*)((vaddr)(&_num_app)
                                               + (i * 2 + 1) * sizeof(u64));
                        app_end_ptr = (u64*)((vaddr)(&_num_app)
                                             + (i * 2 + 2) * sizeof(u64));
                        u64 app_start = *(app_start_ptr);
                        u64 app_end = *(app_end_ptr);

                        error_t e = gen_task_from_elf(
                                NULL, 0, 0, app_start, app_end, NULL);
                        if (e)
                                continue;
                }
        }
        return REND_SUCCESS;
}
