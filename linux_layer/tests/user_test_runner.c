#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/limits.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/task/thread_loader.h>
#include <rendezvos/task/tcb.h>

#include <linux_compat/proc_compat.h>
#include <linux_compat/elf_init.h>
#include <linux_compat/test_runner.h>
#include <modules/test/test.h>
#include <rendezvos/system/powerd.h>

#ifdef LINUX_COMPAT_TEST

extern cpu_id_t BSP_ID;
extern volatile i64 jeffies;
extern u64 _num_app;
extern volatile bool is_print_sche_info;

extern int elf_read_test(void);

/*
 * Improved test framework with better multi-core scalability.
 *
 * Instead of hardcoding RENDEZVOS_MAX_CPU_NUMBER, we use a dynamic
 * approach where each CPU manages its own test slot.
 *
 * For single-core testing: Only CPU 0 slot is used.
 * For multi-core testing: Each CPU uses its own slot based on cpu_number.
 */
#define MAX_CONCURRENT_TESTS 16  /* Reasonable upper bound */

typedef struct {
        volatile u64 cookie;
        volatile i64 exit_code;
        volatile bool in_use;
} test_slot_t;

static test_slot_t linux_test_slots[MAX_CONCURRENT_TESTS];

/*
 * Get test slot for current CPU.
 * For single-core: always returns slot 0.
 * For multi-core: returns hash(cpu_number) % MAX_CONCURRENT_TESTS.
 */
static test_slot_t* get_test_slot(void)
{
        cpu_id_t cpu = percpu(cpu_number);
        /* Simple hash: use modulo to avoid hardcoding MAX_CPUS */
        u32 slot_index = (u32)cpu % MAX_CONCURRENT_TESTS;
        return &linux_test_slots[slot_index];
}

void linux_user_test_notify_exit(i32 owner_cpu, u64 cookie, i64 exit_code)
{
        /*
         * Find the test slot based on owner_cpu.
         * This avoids hardcoding RENDEZVOS_MAX_CPU_NUMBER.
         */
        u32 slot_index = (u32)owner_cpu % MAX_CONCURRENT_TESTS;

        if (slot_index >= MAX_CONCURRENT_TESTS) {
                pr_warn("[linux_user_test] Invalid owner_cpu %d\n", owner_cpu);
                return;
        }

        linux_test_slots[slot_index].exit_code = exit_code;
        linux_test_slots[slot_index].cookie = cookie;
}

static error_t linux_spawn_and_wait_test(u64 app_index)
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
        if (e || !thr) {
                pr_error("[ LINUX USER ] Failed to spawn test %lu: e=%d\n",
                         app_index,
                         (int)e);
                return e ? e : -E_RENDEZVOS;
        }

        linux_thread_append_t *ta = linux_thread_append(thr);
        if (!ta) {
                pr_error(
                        "[ LINUX USER ] Failed to get thread append for test %lu\n",
                        app_index);
                return -E_RENDEZVOS;
        }

        /* Get test slot for this CPU */
        test_slot_t* slot = get_test_slot();

        cpu_id_t cpu = percpu(cpu_number);
        u64 cookie = ((u64)cpu << 56) ^ ((u64)jeffies << 8) ^ (app_index + 1);
        if (cookie == 0)
                cookie = 1;

        ta->test_cookie = cookie;
        slot->cookie = 0;  /* Clear cookie to indicate waiting */
        slot->in_use = true;

        pr_info("[linux_spawn_and_wait_test] CPU %lu: Waiting for test %lu, cookie=0x%lx\n",
                (u64)cpu,
                app_index,
                cookie);

        /* Wait for test completion */
        u64 loop_count = 0;
        while (slot->cookie != cookie) {
                schedule(percpu(core_tm));
                loop_count++;
                if (loop_count % 100000 == 0) {
                        pr_debug(
                                "[linux_spawn_and_wait_test] CPU %lu: Still waiting, loop=%lu, current_cookie=0x%lx, expected=0x%lx\n",
                                (u64)cpu,
                                loop_count,
                                slot->cookie,
                                cookie);
                }
        }

        i64 exit_code = slot->exit_code;
        slot->in_use = false;

        pr_info("[linux_spawn_and_wait_test] CPU %lu: Test %lu completed, cookie matched (0x%lx), exit_code=%ld\n",
                (u64)cpu,
                app_index,
                cookie,
                exit_code);

        return REND_SUCCESS;
}

static void linux_run_user_tests(void)
{
        pr_notice("========================================\n");
        pr_notice(" LINUX USER TEST SUITE START\n");
        pr_notice(" Total tests: %lu\n", _num_app);
        pr_notice("========================================\n");

        /* Disable scheduler debug output to reduce log pollution */
        is_print_sche_info = false;

        int passed = 0;
        int failed = 0;

        for (u64 i = 0; i < _num_app; i++) {
                pr_info("----------------------------------------\n");
                pr_info("[TEST %02lu/%02lu] Starting\n", i + 1, _num_app);
                pr_info("----------------------------------------\n");

                error_t e = linux_spawn_and_wait_test(i);

                if (e) {
                        pr_error("[TEST %02lu/%02lu] FAIL: error=%d\n",
                                 i + 1,
                                 _num_app,
                                 (int)e);
                        failed++;
                } else {
                        pr_info("[TEST %02lu/%02lu] PASS\n", i + 1, _num_app);
                        passed++;
                }
        }

        pr_notice("========================================\n");
        pr_notice(" LINUX USER TEST SUITE DONE\n");
        pr_notice(" Passed: %d/%lu\n", passed, _num_app);
        pr_notice(" Failed: %d/%lu\n", failed, _num_app);
        pr_notice("========================================\n");

        /* Disable scheduler debug output */
        is_print_sche_info = false;
}

static void *linux_user_test_thread(void *arg)
{
        bool is_bsp = (bool)(uintptr_t)arg;

        if (!is_bsp) {
                while (1) {
                        schedule(percpu(core_tm));
                        arch_cpu_relax();
                }
                /* AP: nothing to do */
                return NULL;
        }

        /* BSP: Run Linux compatibility tests */
        pr_info("[ Linux compat ] BSP: Starting ELF read test\n");
        if (elf_read_test() != REND_SUCCESS) {
                pr_error("[ Linux compat ] ELF read test failed\n");
        }

        linux_run_user_tests();

        pr_info("[ Linux compat ] All tests completed\n");

#ifdef RENDEZVOS_ROOT_AUTO_POWEROFF
        pr_info("[ Linux compat ] Requesting shutdown\n");
        (void)rendezvos_request_poweroff();
#endif

        return NULL;
}

static void linux_user_test_init(void)
{
        cpu_id_t cpu = percpu(cpu_number);
        bool is_bsp = (cpu == BSP_ID);

        if (is_bsp) {
                pr_info("[ Linux compat ] BSP: Creating user test thread\n");
                (void)gen_thread_from_func(NULL,
                                           linux_user_test_thread,
                                           "linux_user_test",
                                           percpu(core_tm),
                                           (void *)1);
        } else {
                pr_info("[ Linux compat ] AP CPU %lu: Creating user test thread\n",
                        cpu);
                (void)gen_thread_from_func(NULL,
                                           linux_user_test_thread,
                                           "linux_user_test_ap",
                                           percpu(core_tm),
                                           (void *)0);
        }
}

DEFINE_INIT_LEVEL(linux_user_test_init, 6);

#endif
