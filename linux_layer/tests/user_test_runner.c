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
#include <linux_compat/test_runner.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/page_slice.h>
#include <rendezvos/system/powerd.h>
#include <rendezvos/time.h>

#ifdef LINUX_COMPAT_TEST

/*
 * Single boot path (LINUX_COMPAT_TEST; root undefs RENDEZVOS_TEST):
 *   BSP linux_boot → Path B /init → sh /tests/run_all.sh → cookie wait →
 *   optional RENDEZVOS_ROOT_AUTO_POWEROFF.
 */

extern volatile bool is_print_sche_info;

/* One boot wait slot (PID1 /init); not a per-CPU harness. */
static volatile u64 g_boot_wait_cookie;
static volatile i64 g_boot_wait_exit_code;

void linux_user_test_notify_exit(i32 owner_cpu, u64 cookie, i64 exit_code)
{
        (void)owner_cpu;
        g_boot_wait_exit_code = exit_code;
        g_boot_wait_cookie = cookie;
}

/*
 * Path B spawn + cookie wait until /init (or path) exits via clean_server.
 */
static error_t linux_spawn_and_wait_boot_path(const char *path)
{
        struct allocator *alloc = percpu(kallocator);
        struct page_slice *elf_slice = NULL;
        Thread_Base *thr = NULL;
        error_t e;
        i64 ret;
        u64 cookie;

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

        cookie = ((u64)jeffies_get() << 8) ^ 1ull;
        if (cookie == 0) {
                cookie = 1;
        }

        ta->test_cookie = cookie;
        g_boot_wait_cookie = 0;

        pid_t test_pid = 0;
        Tcb_Base *test_task = thr->belong_tcb;

        if (test_task) {
                test_pid = test_task->pid;
        }

        while (g_boot_wait_cookie != cookie)
                schedule(percpu(core_tm));

        /*
         * Cookie is set at THREAD_REAP (before delete_thread /
         * TASK_REAP). Wait until the task shell is gone so teardown
         * does not race the next boot step.
         */
        if (test_pid > 0) {
                while (find_task_by_pid(test_pid) != NULL)
                        schedule(percpu(core_tm));
        }

        (void)g_boot_wait_exit_code;
        return REND_SUCCESS;
}

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

        is_print_sche_info = false;
        pr_info("[ Linux compat ] Boot: Path B /init → sh /tests/run_all.sh\n");
        if (linux_spawn_and_wait_boot_path("/init") != REND_SUCCESS) {
                pr_error("[ Linux compat ] Boot: /init spawn failed\n");
        } else {
                pr_info("[ Linux compat ] Boot: /init exited\n");
        }

#ifdef RENDEZVOS_ROOT_AUTO_POWEROFF
        pr_info("[ Linux compat ] Requesting shutdown\n");
        (void)rendezvos_request_poweroff();
#endif

        return NULL;
}

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
