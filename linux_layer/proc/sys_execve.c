#include <common/string.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/fault.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/signal/signal_init.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_types.h>
#include <modules/elf/elf.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/map_handler.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/thread_loader.h>
#include <rendezvos/trap/trap.h>
#include <syscall.h>

#if defined(_AARCH64_)
#include <arch/aarch64/tcb_arch.h>
#endif

extern u64 _num_app;

#define EXEC_MAX_PATH       256
#define EXEC_MAX_ARG_LEN    256
#define LINUX_EXEC_MAX_ARGS 128

/*
 * Phase 3a execve: single-threaded, embedded ELF by name, argv on new user
 * stack. Path A return via arch_syscall_set_user_return. No envp/auxv yet.
 */

static i64 find_embedded_elf_by_name(const char *filename)
{
        if (!filename) {
                return -1;
        }

        u64 num_apps = _num_app;

        struct {
                const char *name;
                int index;
        } program_map[] = {
                {"test_echo", 41},
                {"test_execve", 17},
                {"test_execve_simple", 50},
                {"test_signal_delivery", 7},
                {"test_phase2b_signal_basic", 8},
        };

        int num_programs = sizeof(program_map) / sizeof(program_map[0]);

        for (int i = 0; i < num_programs; i++) {
                if (strcmp_s(program_map[i].name, filename, EXEC_MAX_PATH)
                    == 0) {
                        if (program_map[i].index < (i64)num_apps) {
                                return program_map[i].index;
                        }
                }
        }

        return -1;
}

static bool exec_arg_string_valid(const char *buf, size_t cap)
{
        size_t i;

        for (i = 0; i < cap; i++) {
                if (buf[i] == '\0') {
                        return true;
                }
        }
        return false;
}

static i64
linux_exec_copy_argv_from_user(VSpace *vs, u64 user_argv, char *arg_storage,
                               const char *kargv[LINUX_EXEC_MAX_ARGS + 1])
{
        i64 argc = 0;
        error_t e;

        kargv[0] = NULL;
        if (!arg_storage) {
                return -LINUX_ENOMEM;
        }
        if (user_argv == 0) {
                return 0;
        }

        for (i64 i = 0; i < LINUX_EXEC_MAX_ARGS; i++) {
                char *user_arg_ptr;
                char *dst = arg_storage + (size_t)argc * EXEC_MAX_ARG_LEN;

                e = linux_mm_load_from_user(vs,
                                            user_argv + (u64)i * sizeof(char *),
                                            &user_arg_ptr,
                                            sizeof(user_arg_ptr));
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }
                if (user_arg_ptr == NULL) {
                        break;
                }

                e = linux_mm_load_from_user(vs,
                                            (u64)(uintptr_t)user_arg_ptr,
                                            dst,
                                            EXEC_MAX_ARG_LEN);
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }
                dst[EXEC_MAX_ARG_LEN - 1] = '\0';
                if (!exec_arg_string_valid(dst, EXEC_MAX_ARG_LEN)) {
                        return -LINUX_E2BIG;
                }

                kargv[argc] = dst;
                argc++;
        }

        if (argc >= LINUX_EXEC_MAX_ARGS) {
                char *extra;

                e = linux_mm_load_from_user(
                        vs,
                        user_argv + (u64)LINUX_EXEC_MAX_ARGS * sizeof(char *),
                        &extra,
                        sizeof(extra));
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }
                if (extra != NULL) {
                        return -LINUX_E2BIG;
                }
        }

        kargv[argc] = NULL;
        return argc;
}

static error_t exec_store_u64(VSpace *vs, vaddr user_va, u64 value)
{
        return linux_mm_store_to_user(vs, (u64)user_va, &value, sizeof(value));
}

static vaddr build_initial_stack(VSpace *vs, vaddr stack_top, i64 argc,
                                 const char *kargv[], vaddr *argv_user_out)
{
        vaddr sp = stack_top;
        u64 strings_size = 0;
        vaddr strings_base;
        vaddr argv_ptr_area;
        vaddr string_va;
        error_t e;
        i64 i;

        if (argv_user_out) {
                *argv_user_out = 0;
        }

        for (i = 0; i < argc; i++) {
                strings_size += (u64)strlen(kargv[i]) + 1;
        }

        sp -= strings_size;
        sp &= ~((vaddr)0xF);
        strings_base = sp;

        string_va = strings_base;
        for (i = 0; i < argc; i++) {
                size_t len = strlen(kargv[i]) + 1;

                e = linux_mm_store_to_user(vs, string_va, kargv[i], len);
                if (e != REND_SUCCESS) {
                        return 0;
                }
                string_va += (vaddr)len;
        }

        sp -= (u64)(argc + 1) * sizeof(u64);
        sp &= ~((vaddr)0xF);
        argv_ptr_area = sp;

        string_va = strings_base;
        for (i = 0; i < argc; i++) {
                e = exec_store_u64(vs,
                                   argv_ptr_area + (u64)i * sizeof(u64),
                                   (u64)string_va);
                if (e != REND_SUCCESS) {
                        return 0;
                }
                string_va += (vaddr)(strlen(kargv[i]) + 1);
        }

        e = exec_store_u64(vs, argv_ptr_area + (u64)argc * sizeof(u64), 0);
        if (e != REND_SUCCESS) {
                return 0;
        }

        sp -= sizeof(u64);
        e = exec_store_u64(vs, sp, (u64)argc);
        if (e != REND_SUCCESS) {
                return 0;
        }

        if (argv_user_out) {
                *argv_user_out = argv_ptr_area;
        }
        return sp;
}

static void linux_exec_reset_proc_state(Tcb_Base *task)
{
        linux_proc_append_t *pa = linux_proc_append(task);

        if (!pa) {
                return;
        }

        pa->brk = 0;
        pa->start_brk = 0;
        pa->mmap_hint = 0;
        sigemptyset(&pa->pending_signals);
}

/*
 * After vspace_clear_user_mappings succeeds, the old image is gone and exec
 * cannot roll back. Failures must terminate via the common fatal path, not
 * return errno to a trap frame that still points at the old user PC/SP.
 */
static void linux_exec_abort_unrecoverable(struct allocator *alloc,
                                           char *arg_storage, const char *what,
                                           error_t e)
{
        if (alloc && arg_storage) {
                alloc->m_free(alloc, arg_storage);
        }
        pr_error("[EXEC] %s failed after commit (e=%d), terminating task\n",
                 what,
                 (int)e);
        linux_fatal_user_fault(128 + SIGKILL);
}

/*
 * Blind wait until no remote CPU may hold live TLB entries for @p vs.
 * Matches vspace_clear_user_mappings(..., allow_self_use=true): this CPU
 * may still have its bit set while current_vspace==vs.
 */
static void linux_exec_wait_remote_tlb_quiesce(VSpace *vs)
{
        cpu_id_t self = percpu(cpu_number);
        Task_Manager *tm = percpu(core_tm);

        for (;;) {
                bool remote_busy = false;

                lock_cas(&vs->tlb_cpu_mask_lock);
                for (u32 cpu = 0; cpu < (u32)RENDEZVOS_MAX_CPU_NUMBER; cpu++) {
                        if (!BITMAP_OPS(vs_tlb_cpu_bitmap,
                                        test)(&vs->tlb_cpu_mask, cpu)) {
                                continue;
                        }
                        if (cpu != (u32)self) {
                                remote_busy = true;
                                break;
                        }
                }
                unlock_cas(&vs->tlb_cpu_mask_lock);

                if (!remote_busy) {
                        return;
                }

                schedule(tm);
        }
}

i64 sys_execve(struct trap_frame *syscall_ctx, u64 user_filename, u64 user_argv,
               u64 user_envp)
{
        (void)user_envp;

        Tcb_Base *current = get_cpu_current_task();
        Thread_Base *current_thread = get_cpu_current_thread();
        VSpace *vs;
        struct map_handler *handler = &percpu(Map_Handler);
        struct allocator *alloc = percpu(kallocator);
        char filename[EXEC_MAX_PATH];
        char *arg_storage = NULL;
        const char *kargv[LINUX_EXEC_MAX_ARGS + 1];
        error_t e;
        i64 app_index;
        i64 argc;
        vaddr elf_start, elf_end;
        vaddr entry_addr;
        vaddr user_sp;
        vaddr initial_stack_sp;
        vaddr argv_user = 0;

        if (!current || !current_thread || !current->vs) {
                return -LINUX_ESRCH;
        }
        if (!alloc) {
                return -LINUX_ENOMEM;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        e = linux_mm_load_from_user(
                vs, user_filename, filename, sizeof(filename));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }
        filename[EXEC_MAX_PATH - 1] = '\0';

        app_index = find_embedded_elf_by_name(filename);
        if (app_index < 0) {
                return -LINUX_ENOENT;
        }

        elf_start = *(u64 *)((vaddr)(&_num_app)
                             + (app_index * 2 + 1) * (i64)sizeof(u64));
        elf_end = *(u64 *)((vaddr)(&_num_app)
                           + (app_index * 2 + 2) * (i64)sizeof(u64));

        if (!check_elf_header(elf_start)) {
                return -LINUX_ENOEXEC;
        }
        if (get_elf_class(elf_start) != ELFCLASS64) {
                return -LINUX_ENOEXEC;
        }

        arg_storage = alloc->m_alloc(
                alloc, (size_t)LINUX_EXEC_MAX_ARGS * EXEC_MAX_ARG_LEN);
        if (!arg_storage) {
                return -LINUX_ENOMEM;
        }

        argc = linux_exec_copy_argv_from_user(
                vs, user_argv, arg_storage, kargv);
        if (argc < 0) {
                alloc->m_free(alloc, arg_storage);
                return argc;
        }

        /*
         * Phase 3b (partial): blind-wait for remote CPUs to drop vs from
         * tlb_cpu_mask. No de_thread yet; other threads may spin here too.
         */
        linux_exec_wait_remote_tlb_quiesce(vs);

        e = vspace_clear_user_mappings(vs, handler, true);
        if (e != REND_SUCCESS) {
                /*
                 * Quiesce check only: vs untouched, safe to return to caller.
                 * Any other clear error may leave a partially torn vs.
                 */
                if (e == -E_REND_RC_UNEQUAL) {
                        alloc->m_free(alloc, arg_storage);
                        return -LINUX_EAGAIN;
                }
                linux_exec_abort_unrecoverable(
                        alloc, arg_storage, "vspace_clear_user_mappings", e);
        }

        e = load_elf_to_vs(elf_start, elf_end, vs, NULL);
        if (e != REND_SUCCESS) {
                linux_exec_abort_unrecoverable(
                        alloc, arg_storage, "load_elf_to_vs", e);
        }

        entry_addr = ((Elf64_Ehdr *)elf_start)->e_entry;

        user_sp = generate_user_stack(vs);
        if (!user_sp) {
                linux_exec_abort_unrecoverable(alloc,
                                               arg_storage,
                                               "generate_user_stack",
                                               -E_RENDEZVOS);
        }

        initial_stack_sp =
                build_initial_stack(vs, user_sp, argc, kargv, &argv_user);
        alloc->m_free(alloc, arg_storage);
        arg_storage = NULL;
        if (initial_stack_sp == 0) {
                linux_exec_abort_unrecoverable(
                        alloc, NULL, "build_initial_stack", -E_RENDEZVOS);
        }

        linux_exec_reset_proc_state(current);
        linux_signal_reset_thread_handler_state(
                linux_thread_append(current_thread));

        arch_syscall_set_user_return(syscall_ctx,
                                     &current_thread->ctx,
                                     entry_addr,
                                     initial_stack_sp,
                                     0);
#if defined(_AARCH64_)
        /*
         * Linux aarch64 process start: x0=argc, x1=argv (user VA).
         * Overwrites REGS[0] after syscall_ret=0 was stored.
         */
        arch_syscall_set_user_int_arg(syscall_ctx, 0, (u64)argc);
        arch_syscall_set_user_int_arg(syscall_ctx, 1, (u64)argv_user);
#endif

        return 0;
}
