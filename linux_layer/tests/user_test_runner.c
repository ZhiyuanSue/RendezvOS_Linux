#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/limits.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/task/thread_loader.h>
#include <rendezvos/task/tcb.h>

#include <common/stdbool.h>
#include <common/stddef.h>

#include <linux_compat/proc_compat.h>
#include <linux_compat/elf_init.h>
#include <linux_compat/test_runner.h>

#ifdef LINUX_COMPAT_TEST

extern cpu_id_t BSP_ID;
extern int NR_CPU;
extern volatile i64 jeffies;

extern u64 _num_app;

/* elf_read_test is kernel-side; keep BSP-only. */
extern int elf_read_test(void);

enum linux_user_case_phase {
        LINUX_CASE_IDLE = 0,
        LINUX_CASE_BEGIN = 1,
        LINUX_CASE_END = 2,
};

static volatile u64 linux_case_epoch;
static volatile u64 linux_case_index;
static volatile enum linux_user_case_phase linux_case_phase;

static volatile u64 linux_case_begin_arrived[RENDEZVOS_MAX_CPU_NUMBER];
static volatile u64 linux_case_end_arrived[RENDEZVOS_MAX_CPU_NUMBER];

static volatile u64 linux_user_done_cookie[RENDEZVOS_MAX_CPU_NUMBER];
static volatile i64 linux_user_done_code[RENDEZVOS_MAX_CPU_NUMBER];

void linux_user_test_notify_exit(i32 owner_cpu, u64 cookie, i64 exit_code)
{
        if (owner_cpu < 0 || owner_cpu >= (i32)RENDEZVOS_MAX_CPU_NUMBER)
                return;
        linux_user_done_code[owner_cpu] = exit_code;
        linux_user_done_cookie[owner_cpu] = cookie;
}

static void linux_case_barrier_begin(u64 epoch)
{
        i32 cpu = (i32)percpu(cpu_number);
        linux_case_begin_arrived[cpu] = epoch;
        while (1) {
                bool all = true;
                for (i32 i = 0; i < NR_CPU; i++) {
                        if (linux_case_begin_arrived[i] != epoch) {
                                all = false;
                                break;
                        }
                }
                if (all)
                        return;
                schedule(percpu(core_tm));
        }
}

static void linux_case_barrier_end(u64 epoch)
{
        i32 cpu = (i32)percpu(cpu_number);
        linux_case_end_arrived[cpu] = epoch;
        while (1) {
                bool all = true;
                for (i32 i = 0; i < NR_CPU; i++) {
                        if (linux_case_end_arrived[i] != epoch) {
                                all = false;
                                break;
                        }
                }
                if (all)
                        return;
                schedule(percpu(core_tm));
        }
}

static error_t linux_spawn_one_app_and_wait(u64 app_index)
{
        u64 *app_start_ptr =
                (u64 *)((vaddr)(&_num_app) + (app_index * 2 + 1) * sizeof(u64));
        u64 *app_end_ptr =
                (u64 *)((vaddr)(&_num_app) + (app_index * 2 + 2) * sizeof(u64));
        u64 app_start = *(app_start_ptr);
        u64 app_end = *(app_end_ptr);

        Thread_Base *thr = NULL;
        error_t e = gen_task_from_elf(&thr,
                                      LINUX_PROC_APPEND_BYTES,
                                      LINUX_THREAD_APPEND_BYTES,
                                      app_start,
                                      app_end,
                                      linux_elf_init_handler);
        if (e || !thr)
                return e ? e : -E_RENDEZVOS;

        linux_thread_append_t *ta = linux_thread_append(thr);
        if (!ta)
                return -E_RENDEZVOS;

        i32 cpu = (i32)percpu(cpu_number);
        u64 cookie = ((u64)cpu << 56) ^ ((u64)jeffies << 8) ^ (app_index + 1);
        if (cookie == 0)
                cookie = 1;
        ta->test_cookie = cookie;
        linux_user_done_cookie[cpu] = 0;
        linux_user_done_code[cpu] = 0;

        /* Wait until clean_server notifies the cookie for this CPU. */
        while (linux_user_done_cookie[cpu] != cookie) {
                schedule(percpu(core_tm));
        }
        return REND_SUCCESS;
}

static void linux_user_tests_single_phase(void)
{
        pr_notice("====== [ LINUX USER TEST SINGLE ] ======\n");
        for (u64 i = 0; i < _num_app; i++) {
                pr_notice("[ LINUX USER SINGLE case=%lu ]\n", i);
                error_t e = linux_spawn_one_app_and_wait(i);
                if (e) {
                        pr_error("[ LINUX USER SINGLE ] case=%lu failed e=%d\n",
                                 i,
                                 (int)e);
                        break;
                }
        }
        pr_notice("====== [ LINUX USER TEST SINGLE DONE ] ======\n");
}

static void linux_user_tests_smp_phase_ap(void)
{
        while (linux_case_phase == LINUX_CASE_IDLE) {
                schedule(percpu(core_tm));
        }
        while (1) {
                u64 epoch = linux_case_epoch;
                if (linux_case_phase == LINUX_CASE_IDLE)
                        break;

                if (linux_case_phase == LINUX_CASE_BEGIN) {
                        linux_case_barrier_begin(epoch);
                        (void)linux_spawn_one_app_and_wait(linux_case_index);
                        linux_case_barrier_end(epoch);
                } else {
                        schedule(percpu(core_tm));
                }
        }
}

static void linux_user_tests_smp_phase_bsp(void)
{
        pr_notice("====== [ LINUX USER TEST SMP ] ======\n");
        for (u64 i = 0; i < _num_app; i++) {
                linux_case_index = i;
                linux_case_epoch++;
                linux_case_phase = LINUX_CASE_BEGIN;
                pr_notice("[ LINUX USER SMP case=%lu begin ]\n", i);
                linux_case_barrier_begin(linux_case_epoch);

                (void)linux_spawn_one_app_and_wait(i);

                linux_case_barrier_end(linux_case_epoch);
                pr_notice("[ LINUX USER SMP case=%lu end ]\n", i);
                linux_case_phase = LINUX_CASE_END;
                schedule(percpu(core_tm));
        }
        linux_case_phase = LINUX_CASE_IDLE;
        pr_notice("====== [ LINUX USER TEST SMP DONE ] ======\n");
}

static void *linux_user_test_runner_thread(void *arg)
{
        bool is_bsp = (bool)(uintptr_t)arg;
        if (is_bsp) {
                pr_info("[ Linux compat ] running elf_read_test (BSP)\n");
                if (elf_read_test() != REND_SUCCESS) {
                        pr_error("[ Linux compat ] elf_read_test failed\n");
                }
                linux_user_tests_single_phase();

                /* Kick SMP phase. */
                linux_case_epoch = 1;
                linux_case_phase = LINUX_CASE_BEGIN;
                linux_user_tests_smp_phase_bsp();
        } else {
                linux_user_tests_smp_phase_ap();
        }
        thread_set_status(percpu(init_thread_ptr), thread_status_ready);
        schedule(percpu(core_tm));
        return NULL;
}

static void linux_user_test_runner_init(void)
{
        cpu_id_t cpu = percpu(cpu_number);
        if (cpu == BSP_ID) {
                pr_info("[ Linux compat ] start user test runner (BSP)\n");
                (void)gen_thread_from_func(NULL,
                                           linux_user_test_runner_thread,
                                           "linux_user_test_bsp",
                                           percpu(core_tm),
                                           (void *)1);
        } else {
                pr_info("[ Linux compat ] start user test runner (AP)\n");
                (void)gen_thread_from_func(NULL,
                                           linux_user_test_runner_thread,
                                           "linux_user_test_ap",
                                           percpu(core_tm),
                                           (void *)0);
        }
}

DEFINE_INIT_LEVEL(linux_user_test_runner_init, 6);

#endif
