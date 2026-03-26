#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/task/thread_loader.h>
#include <rendezvos/smp/percpu.h>

extern u64 _num_app;
#define NR_MAX_TEST NEXUS_PER_PAGE * 3
extern void* test_ptrs[NR_MAX_TEST];
int task_test(void)
{
        // if (percpu(cpu_number) == 0) {
                // pr_info("%x apps\n", _num_app);
                // u64* app_start_ptr;
                // u64* app_end_ptr;
                // for (u64 i = 0; i < _num_app; i++) {
                //         app_start_ptr = (u64*)((vaddr)(&_num_app)
                //                                + (i * 2 + 1) * sizeof(u64));
                //         app_end_ptr = (u64*)((vaddr)(&_num_app)
                //                              + (i * 2 + 2) * sizeof(u64));
                //         u64 app_start = *(app_start_ptr);
                //         u64 app_end = *(app_end_ptr);

                //         error_t e = gen_task_from_elf(
                //                 NULL, 0, 0, app_start, app_end, NULL);
                //         if (e)
                //                 continue;
                // }
        // }
        if (percpu(cpu_number) == 1) {
                pr_info("start task test\n");
                error_t e = REND_SUCCESS;
                VS_Common* vs = new_vspace();
                if (!vs) {
                        e = -E_REND_TEST;
                        return e;
                }
                paddr new_vs_paddr = new_vs_root(0, &percpu(Map_Handler));
                if (!new_vs_paddr) {
                        e = -E_REND_TEST;
                        return e;
                }
                set_vspace_root_addr(vs, new_vs_paddr);
                struct nexus_node* new_vs_nexus_root =
                        nexus_create_vspace_root_node(percpu(nexus_root), vs);
                init_vspace(vs, get_new_id(&pid_manager), new_vs_nexus_root);

                int page_num = 2;
                vaddr start_test_addr = PAGE_SIZE;
                get_free_page(page_num,
                              start_test_addr,
                              percpu(nexus_root),
                              vs,
                              PAGE_ENTRY_READ | PAGE_ENTRY_VALID
                                      | PAGE_ENTRY_WRITE);
                // e = del_vspace(&vs);
                // if(e)
                //         return e;
        }
        return REND_SUCCESS;
}
