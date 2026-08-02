#include <common/string.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/system/powerd.h>
#include <rendezvos/task/id.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/thread_loader.h>
#include <rendezvos/time.h>

#include <linux_compat/append_hooks.h>
#include <linux_compat/boot_wait.h>
#include <linux_compat/errno.h>
#include <linux_compat/fs/vfs_root_bootstrap.h>
#include <linux_compat/initcall.h>
#include <linux_compat/proc/linux_exec.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>

#if defined(_X86_64_)
#include <arch/x86_64/tcb_arch.h>
#elif defined(_AARCH64_)
#include <arch/aarch64/tcb_arch.h>
#endif

#ifdef LINUX_COMPAT_TEST

/*
 * Kernel PID1 launcher:
 *   empty user task → linux_exec_replace_image("/init", argv) → drop to user
 * Default argv (no cmdline yet): sh /tests/run_all.sh
 */

extern volatile bool is_print_sche_info;
extern VSpace root_vspace;

static volatile u64 g_boot_wait_cookie;
static volatile i64 g_boot_wait_exit_code;

void linux_boot_notify_exit(i32 owner_cpu, u64 cookie, i64 exit_code)
{
        (void)owner_cpu;
        g_boot_wait_exit_code = exit_code;
        g_boot_wait_cookie = cookie;
}

static const char *const linux_init_default_argv[] = {
        "sh",
        "/tests/run_all.sh",
        NULL,
};

static void linux_boot_release_task(Tcb_Base *task)
{
        if (!task) {
                return;
        }
        if (task->vs && task->vs != task->vs->root_vs) {
                VSpace *vs = task->vs;

                task->vs = NULL;
                ref_put(&vs->refcount, free_vspace_ref);
        }
        (void)delete_task(task);
}

/*
 * First kernel body of PID1: attach Linux state, exec /init (same stack/auxv
 * as sys_execve), then Path B drop to user.
 */
static void linux_run_init_exec(void)
{
        Thread_Base *self = get_cpu_current_thread();
        Tcb_Base *task = get_cpu_current_task();
        vaddr entry = 0;
        vaddr sp = 0;
        i64 ret;
        struct trap_frame drop_tf;

        if (!self || !task || !task->vs) {
                pr_error("[ LINUX BOOT ] init exec: missing task/thread\n");
                goto hang;
        }

        if (linux_user_task_prepare_new(task, self) != REND_SUCCESS) {
                pr_error("[ LINUX BOOT ] init exec: prepare_new failed\n");
                goto hang;
        }

        ret = linux_exec_replace_image(task,
                                       self,
                                       "/init",
                                       2,
                                       linux_init_default_argv,
                                       false,
                                       &entry,
                                       &sp);
        if (ret != 0 || entry == 0 || sp == 0) {
                pr_error("[ LINUX BOOT ] exec /init failed: %lld\n",
                         (long long)ret);
                goto hang;
        }

        arch_empty_drop_trap_frame(&drop_tf, entry);
        arch_syscall_set_user_return(
                &drop_tf, &self->ctx, entry, sp, 0);
        arch_return_to_user(self->kstack_bottom, &drop_tf, 0);

hang:
        for (;;) {
                schedule(percpu(core_tm));
        }
}

/*
 * Create empty user task/thread. Stamps boot_wait_cookie before thread_join
 * so exit cannot race the waiter.
 */
static error_t linux_spawn_init_task(Thread_Base **out_thr, u64 *cookie_out)
{
        Tcb_Base *task;
        Thread_Base *thr;
        linux_thread_append_t *ta;
        error_t e;
        u64 cookie;

        if (out_thr) {
                *out_thr = NULL;
        }
        if (cookie_out) {
                *cookie_out = 0;
        }

        task = new_task_structure(percpu(kallocator), &linux_task_append_hooks);
        if (!task) {
                return -E_RENDEZVOS;
        }

        task->pid = get_new_id(&pid_manager);
        task->vs = create_vspace(root_vspace.pmm);
        if (!task->vs) {
                (void)delete_task(task);
                return -E_RENDEZVOS;
        }

        e = register_vspace(task->vs, &root_vspace, task->pid);
        if (e != REND_SUCCESS) {
                linux_boot_release_task(task);
                return e;
        }

        e = add_task_to_manager(percpu(core_tm), task);
        if (e != REND_SUCCESS) {
                linux_boot_release_task(task);
                return e;
        }

        thr = create_thread((void *)linux_run_init_exec,
                            &linux_thread_append_hooks,
                            true,
                            0);
        if (!thr) {
                (void)del_task_from_manager(task);
                linux_boot_release_task(task);
                return -E_RENDEZVOS;
        }

        thread_set_flags(thr, THREAD_FLAG_USER);
        ta = linux_thread_append(thr);
        if (!ta) {
                del_thread_structure(thr);
                (void)del_task_from_manager(task);
                linux_boot_release_task(task);
                return -E_RENDEZVOS;
        }

        cookie = ((u64)jeffies_get() << 8) ^ 1ull;
        if (cookie == 0) {
                cookie = 1;
        }
        ta->boot_wait_cookie = cookie;
        g_boot_wait_cookie = 0;

        e = thread_join(task, thr);
        if (e != REND_SUCCESS) {
                del_thread_from_manager(thr);
                del_thread_structure(thr);
                (void)del_task_from_manager(task);
                linux_boot_release_task(task);
                return e;
        }

        if (out_thr) {
                *out_thr = thr;
        }
        if (cookie_out) {
                *cookie_out = cookie;
        }
        return REND_SUCCESS;
}

static error_t linux_spawn_and_wait_init(void)
{
        Thread_Base *thr = NULL;
        u64 cookie = 0;
        pid_t boot_pid = 0;
        error_t e;

        e = linux_spawn_init_task(&thr, &cookie);
        if (e != REND_SUCCESS || !thr || cookie == 0) {
                pr_error("[ LINUX BOOT ] spawn init task failed e=%d\n",
                         (int)e);
                return e ? e : -E_RENDEZVOS;
        }

        if (thr->belong_tcb) {
                boot_pid = thr->belong_tcb->pid;
        }

        while (g_boot_wait_cookie != cookie)
                schedule(percpu(core_tm));

        if (boot_pid > 0) {
                while (find_task_by_pid(boot_pid) != NULL)
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
        pr_info("[ Linux compat ] Boot: exec /init → sh /tests/run_all.sh\n");
        if (linux_spawn_and_wait_init() != REND_SUCCESS) {
                pr_error("[ Linux compat ] Boot: /init failed\n");
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
