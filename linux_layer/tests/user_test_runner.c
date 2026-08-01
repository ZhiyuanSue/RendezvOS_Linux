#include <common/string.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/limits.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/task/thread_loader.h>
#include <rendezvos/task/tcb.h>

#include <linux_compat/append_hooks.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/vfs_kern_load.h>
#include <linux_compat/fs/vfs_root_bootstrap.h>
#include <linux_compat/initcall.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/test_manifest.h>
#include <linux_compat/test_runner.h>
#include <modules/test/test.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/page_slice.h>
#include <rendezvos/system/powerd.h>
#include <rendezvos/time.h>

#ifdef LINUX_COMPAT_TEST

/*
 * Boot path (BUSYBOX_BOOT_DEFERRALS):
 *   BSP → Path B /init (= busybox symlink) once → sh /tests/run_all.sh.
 * argv comes from linux_exec_bootstrap (glibc busybox needs auxv stack).
 *
 * Set to 0 to restore the old kernel-orchestrated manifest suite + demo.
 */
#ifndef LINUX_COMPAT_BOOT_BUSYBOX_ONLY
#define LINUX_COMPAT_BOOT_BUSYBOX_ONLY 1
#endif

extern volatile bool is_print_sche_info;

extern int elf_read_test(void);

#define MAX_CONCURRENT_TESTS 16

typedef struct {
        volatile u64 cookie;
        volatile i64 exit_code;
        volatile bool in_use;
} test_slot_t;

static test_slot_t linux_test_slots[MAX_CONCURRENT_TESTS];

static test_slot_t *get_test_slot(void)
{
        cpu_id_t cpu = percpu(cpu_number);
        u32 slot_index = (u32)cpu % MAX_CONCURRENT_TESTS;
        return &linux_test_slots[slot_index];
}

void linux_user_test_notify_exit(i32 owner_cpu, u64 cookie, i64 exit_code)
{
        u32 slot_index = (u32)owner_cpu % MAX_CONCURRENT_TESTS;

        if (slot_index >= MAX_CONCURRENT_TESTS) {
                pr_warn("[linux_user_test] Invalid owner_cpu %d\n", owner_cpu);
                return;
        }

        linux_test_slots[slot_index].exit_code = exit_code;
        linux_test_slots[slot_index].cookie = cookie;
}

/*
 * Path B spawn + cookie wait (shared by boot and legacy suite).
 * For busybox, argv/auxv are injected in thread.append.init bootstrap.
 */
static error_t linux_spawn_and_wait_test_path(const char *path, u32 test_index)
{
        struct allocator *alloc = percpu(kallocator);
        struct page_slice *elf_slice = NULL;
        Thread_Base *thr = NULL;
        error_t e;
        i64 ret;

        if (!path || !alloc) {
                return -E_IN_PARAM;
        }

        ret = vfs_kern_read_file_slice(path, alloc, &elf_slice);
        if (ret < 0 || !elf_slice) {
                pr_error(
                        "[ LINUX USER ] Failed to read test slice '%s': %lld\n",
                        path,
                        (long long)ret);
                if (ret == -LINUX_ENOENT && path
                    && strcmp_s(path, "/init", 8) == 0) {
                        pr_error(
                                "[ LINUX USER ] hint: /init missing from cpio — "
                                "run 'make user' (busybox installs init→busybox)\n");
                }
                return (error_t)ret;
        }

        e = gen_task_from_elf(&thr,
                              &linux_task_append_hooks,
                              &linux_thread_append_hooks,
                              elf_slice);

        if (e != REND_SUCCESS || !thr) {
                page_slice_destroy(&elf_slice);
                pr_error("[ LINUX USER ] Failed to spawn '%s': e=%d\n",
                         path,
                         (int)e);
                return e ? e : -E_RENDEZVOS;
        }

        linux_thread_append_t *ta = linux_thread_append(thr);
        if (!ta) {
                pr_error("[ LINUX USER ] No thread append for '%s'\n", path);
                return -E_RENDEZVOS;
        }

        test_slot_t *slot = get_test_slot();
        cpu_id_t cpu = percpu(cpu_number);
        u64 cookie = ((u64)cpu << 56) ^ ((u64)jeffies_get() << 8)
                     ^ ((u64)test_index + 1);
        if (cookie == 0) {
                cookie = 1;
        }

        ta->test_cookie = cookie;
        slot->cookie = 0;
        slot->in_use = true;

        pid_t test_pid = 0;
        Tcb_Base *test_task = thr->belong_tcb;

        if (test_task) {
                test_pid = test_task->pid;
        }

        while (slot->cookie != cookie)
                schedule(percpu(core_tm));

        /*
         * Cookie is set at THREAD_REAP (before delete_thread /
         * TASK_REAP). Wait until the task shell is gone so the next
         * spawn does not run page_slice_insert_page concurrently with
         * del_vspace on the previous test.
         */
        if (test_pid > 0) {
                while (find_task_by_pid(test_pid) != NULL)
                        schedule(percpu(core_tm));
        }

        slot->in_use = false;

        return REND_SUCCESS;
}

#if !LINUX_COMPAT_BOOT_BUSYBOX_ONLY
static void linux_run_user_tests(void)
{
        u32 total = linux_user_test_count();
        u32 i;
        int passed = 0;
        int failed = 0;

        pr_notice("========================================\n");
        pr_notice(" LINUX USER TEST SUITE START (initramfs)\n");
        pr_notice(" Total tests: %u\n", total);
        pr_notice("========================================\n");

        is_print_sche_info = false;

        for (i = 0; i < total; i++) {
                const char *path = linux_user_test_path(i);

                pr_info("----------------------------------------\n");
                pr_info("[TEST %02u/%02u] Starting: %s\n", i + 1, total, path);
                pr_info("----------------------------------------\n");

                error_t e = linux_spawn_and_wait_test_path(path, i);

                if (e != REND_SUCCESS) {
                        pr_error("[TEST %02u/%02u] FAIL: error=%d\n",
                                 i + 1,
                                 total,
                                 (int)e);
                        failed++;
                } else {
                        pr_info("[TEST %02u/%02u] PASS\n", i + 1, total);
                        passed++;
                }
        }

        pr_notice("========================================\n");
        pr_notice(" LINUX USER TEST SUITE DONE\n");
        pr_notice(" Passed: %d/%u\n", passed, total);
        pr_notice(" Failed: %d/%u\n", failed, total);
        pr_notice("========================================\n");

        is_print_sche_info = false;
}
#endif /* !LINUX_COMPAT_BOOT_BUSYBOX_ONLY */

/*
 * BSP boot thread: VFS ready → one Path B busybox → optional poweroff.
 * Legacy name/file kept; thread name is linux_boot.
 */
static void *linux_boot_thread(void *arg)
{
        bool is_bsp = (bool)(uintptr_t)arg;
        error_t err;

        (void)arg;
        if (!is_bsp) {
                return NULL;
        }

        pr_info("[ Linux compat ] BSP: boot thread running (CPU %llu)\n",
                (u64)percpu(cpu_number));

        err = linux_vfs_root_ensure_init();
        if (err != REND_SUCCESS) {
                pr_error(
                        "[ Linux compat ] linux_vfs_root_ensure_init failed: %d\n",
                        (int)err);
                return NULL;
        }

        linux_vfs_wait_backends_ready();

#if LINUX_COMPAT_BOOT_BUSYBOX_ONLY
        /*
         * Path B /init (=busybox); bootstrap argv runs /tests/run_all.sh.
         */
        is_print_sche_info = false;
        pr_info("[ Linux compat ] Boot: Path B /init → sh /tests/run_all.sh\n");
        if (linux_spawn_and_wait_test_path("/init", 0u) != REND_SUCCESS) {
                pr_error("[ Linux compat ] Boot: /init spawn failed\n");
        } else {
                pr_info("[ Linux compat ] Boot: /init exited\n");
        }
#else
        err = (error_t)linux_user_test_load_manifest();
        if (err != REND_SUCCESS) {
                pr_error("[ Linux compat ] manifest load failed: %d\n",
                         (int)err);
                return NULL;
        }

        pr_info("[ Linux compat ] BSP: Starting ELF read test\n");
        if (elf_read_test() != REND_SUCCESS) {
                pr_error("[ Linux compat ] ELF read test failed\n");
        }

        linux_run_user_tests();

        pr_info("[ Linux compat ] Trying initramfs /bin/busybox demo\n");
        if (linux_spawn_and_wait_test_path("/bin/busybox", 9998u)
            != REND_SUCCESS) {
                pr_error("[ Linux compat ] /bin/busybox demo spawn failed\n");
        }

        pr_info("[ Linux compat ] All tests completed\n");
#endif

#ifdef RENDEZVOS_ROOT_AUTO_POWEROFF
        pr_info("[ Linux compat ] Requesting shutdown\n");
        (void)rendezvos_request_poweroff();
#endif

        return NULL;
}

/*
 * BSP-only boot / harness entry. Global resources (VFS root, proc registry)
 * are BSP-once; see initcall.h.
 */
static void linux_boot_init(void)
{
        error_t err;

        if (!linux_init_on_bsp()) {
                return;
        }

        pr_info("[ Linux compat ] BSP: creating boot thread (pre-SMP)\n");
        err = gen_thread_from_func(NULL,
                                   linux_boot_thread,
                                   "linux_boot",
                                   percpu(core_tm),
                                   (void *)1);
        if (err != REND_SUCCESS) {
                pr_error(
                        "[ Linux compat ] BSP: boot thread create failed: %d\n",
                        (int)err);
        }
}

DEFINE_INIT_LEVEL(linux_boot_init, 6);

#endif /* LINUX_COMPAT_TEST */
