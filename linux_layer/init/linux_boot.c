#include <common/string.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/vmm.h>
#include <common/refcount.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/system/powerd.h>
#include <rendezvos/task/id.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/task/thread.h>
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
#include <arch/x86_64/thread_arch.h>
#elif defined(_AARCH64_)
#include <arch/aarch64/thread_arch.h>
#endif

#ifdef LINUX_COMPAT_TEST

/*
 * Kernel PID1 launcher:
 *   empty user task → linux_exec_replace_image("/init", argv) → drop to user
 * argv: core cmdline_ptr (QEMU -append / bootargs), else sh /tests/run_all.sh
 */

extern volatile bool is_print_sche_info;
extern VSpace root_vspace;
/* Core boot: multiboot cmdline / DTB chosen.bootargs (stable for kernel life). */
extern char *cmdline_ptr;

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

/* Mutable copy for whitespace split; points into this for replace_image. */
static char linux_boot_cmdline_buf[4096];
static const char *linux_boot_argv_ptrs[LINUX_EXEC_MAX_ARGS + 1];

/*
 * Build argv for /init from core cmdline_ptr (space-separated tokens).
 * Returns argc (>= 1) and sets *@argv_out; on empty cmdline uses default.
 */
static i64 linux_boot_resolve_argv(const char *const **argv_out)
{
        const char *src;
        char *p;
        i64 argc;
        u64 n;
        u64 i;
        bool in_token;

        if (!argv_out) {
                return -1;
        }

        src = cmdline_ptr;
        if (!src || !src[0]) {
                *argv_out = linux_init_default_argv;
                return 2;
        }

        n = strlen(src);
        if (n >= sizeof(linux_boot_cmdline_buf)) {
                n = sizeof(linux_boot_cmdline_buf) - 1;
                pr_error("[ LINUX BOOT ] cmdline truncated to %llu bytes\n",
                         (unsigned long long)n);
        }
        memcpy(linux_boot_cmdline_buf, src, (size_t)n);
        linux_boot_cmdline_buf[n] = '\0';

        argc = 0;
        in_token = false;
        p = linux_boot_cmdline_buf;
        for (i = 0; i <= n; i++) {
                char c = linux_boot_cmdline_buf[i];

                if (c == '\0' || c == ' ' || c == '\t' || c == '\n'
                    || c == '\r') {
                        if (in_token) {
                                linux_boot_cmdline_buf[i] = '\0';
                                if (argc < LINUX_EXEC_MAX_ARGS) {
                                        linux_boot_argv_ptrs[argc++] = p;
                                }
                                in_token = false;
                        }
                        continue;
                }
                if (!in_token) {
                        p = &linux_boot_cmdline_buf[i];
                        in_token = true;
                }
        }

        if (argc == 0) {
                *argv_out = linux_init_default_argv;
                return 2;
        }

        linux_boot_argv_ptrs[argc] = NULL;
        *argv_out = linux_boot_argv_ptrs;
        return argc;
}

static void linux_boot_release_spawn(Thread_Base *thr, linux_proc_resource_t *proc,
                                     VSpace *vs)
{
        if (thr)
                del_thread_structure(thr);
        if (vs && vs != &root_vspace)
                (void)ref_put(&vs->refcount, free_vspace_ref);
        if (proc)
                (void)linux_proc_put(proc);
}

/*
 * First kernel body of PID1: attach Linux state, exec /init (same stack/auxv
 * as sys_execve), then Path B drop to user.
 */
static void linux_run_init_exec(void)
{
        Thread_Base *self = get_cpu_current_thread();
        linux_proc_resource_t *task = linux_proc_of(self);
        vaddr entry = 0;
        vaddr sp = 0;
        i64 ret;
        i64 argc;
        const char *const *argv;
        struct trap_frame drop_tf;

        if (!self || !task || !self->vs) {
                pr_error("[ LINUX BOOT ] init exec: missing task/thread\n");
                goto hang;
        }

        if (linux_user_task_prepare_new(task, self) != REND_SUCCESS) {
                pr_error("[ LINUX BOOT ] init exec: prepare_new failed\n");
                goto hang;
        }

        argc = linux_boot_resolve_argv(&argv);
        if (argc < 1 || !argv) {
                pr_error("[ LINUX BOOT ] init exec: bad argv\n");
                goto hang;
        }
        pr_info("[ LINUX BOOT ] exec /init argc=%lld argv0=%s\n",
                (long long)argc,
                argv[0] ? argv[0] : "(null)");

        ret = linux_exec_replace_image(task,
                                       self,
                                       "/init",
                                       argc,
                                       argv,
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
 * Create empty user thread + proc. Stamps boot_wait_cookie before
 * add_thread_to_manager so exit cannot race the waiter.
 */
static error_t linux_spawn_init_task(Thread_Base **out_thr, u64 *cookie_out)
{
        linux_proc_resource_t *proc;
        Thread_Base *thr;
        linux_thread_append_t *ta;
        VSpace *vs;
        error_t e;
        u64 cookie;

        if (out_thr) {
                *out_thr = NULL;
        }
        if (cookie_out) {
                *cookie_out = 0;
        }

        proc = linux_proc_alloc();
        if (!proc) {
                return -E_RENDEZVOS;
        }

        vs = create_vspace(root_vspace.pmm);
        if (!vs) {
                (void)linux_proc_put(proc);
                return -E_RENDEZVOS;
        }

        e = register_vspace(vs, &root_vspace);
        if (e != REND_SUCCESS) {
                linux_boot_release_spawn(NULL, proc, vs);
                return e;
        }

        thr = create_thread((void *)linux_run_init_exec,
                            &linux_thread_append_hooks,
                            vs,
                            true,
                            0);
        if (!thr) {
                linux_boot_release_spawn(NULL, proc, vs);
                return -E_RENDEZVOS;
        }
        vs = NULL;

        thread_set_flags(thr, THREAD_FLAG_USER);

        if (linux_proc_attach_thread(proc, thr) != REND_SUCCESS) {
                linux_boot_release_spawn(thr, proc, NULL);
                return -E_RENDEZVOS;
        }

        ta = linux_thread_append(thr);
        if (!ta) {
                linux_boot_release_spawn(thr, proc, NULL);
                return -E_RENDEZVOS;
        }

        cookie = ((u64)jeffies_get() << 8) ^ 1ull;
        if (cookie == 0) {
                cookie = 1;
        }
        ta->boot_wait_cookie = cookie;
        g_boot_wait_cookie = 0;

        e = add_thread_to_manager(percpu(core_tm), thr);
        if (e != REND_SUCCESS) {
                linux_boot_release_spawn(thr, proc, NULL);
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

        {
                linux_proc_resource_t *proc = linux_proc_of(thr);

                if (proc) {
                        boot_pid = proc->pid;
                }
        }

        while (g_boot_wait_cookie != cookie)
                schedule(percpu(core_tm));

        if (boot_pid > 0) {
                while (find_proc_by_pid(boot_pid) != NULL)
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
        pr_info("[ Linux compat ] Boot: exec /init (cmdline or default argv)\n");
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
