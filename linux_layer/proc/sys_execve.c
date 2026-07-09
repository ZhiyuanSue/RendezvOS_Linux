#include <common/string.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/fault.h>
#include <linux_compat/fs/linux_exec_image.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/mm/linux_page_slice_file.h>
#include <linux_compat/proc/linux_exec_proc.h>
#include <linux_compat/signal/signal_init.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_types.h>
#include <modules/elf/elf.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/map_handler.h>
#include <rendezvos/mm/page_slice.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/thread_loader.h>
#include <rendezvos/trap/trap.h>
#include <syscall.h>

#if defined(_AARCH64_)
#include <arch/aarch64/tcb_arch.h>
#endif

#define EXEC_MAX_PATH       256
#define EXEC_MAX_ARG_LEN    256
#define LINUX_EXEC_MAX_ARGS 128

/*
 * Phase 3 execve: embedded ELF by basename, or static ELF64 from VFS path.
 * argv on new user stack. Path A return via arch_syscall_set_user_return.
 * No envp/auxv yet. No PT_INTERP / dynamic linking.
 */

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

/*
 * After vspace_clear_user_mappings succeeds, the old image is gone and exec
 * cannot roll back. Failures must terminate via the common fatal path, not
 * return errno to a trap frame that still points at the old user PC/SP.
 */
static void linux_exec_abort_unrecoverable(struct allocator *alloc,
                                           char *arg_storage,
                                           struct page_slice *elf_slice,
                                           const char *what, error_t e)
{
        if (alloc && arg_storage) {
                alloc->m_free(alloc, arg_storage);
        }
        if (elf_slice) {
                page_slice_destroy(&elf_slice);
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
        struct page_slice *elf_slice = NULL;
        const char *kargv[LINUX_EXEC_MAX_ARGS + 1];
        error_t e;
        i64 argc;
        i64 ret;
        vaddr max_load_end = 0;
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

        ret = linux_exec_load_elf_slice(vs, filename, alloc, &elf_slice);
        if (ret != 0) {
                return ret;
        }

        if (!linux_exec_elf_slice_valid(elf_slice)) {
                page_slice_destroy(&elf_slice);
                return -LINUX_ENOEXEC;
        }

        arg_storage = alloc->m_alloc(
                alloc, (size_t)LINUX_EXEC_MAX_ARGS * EXEC_MAX_ARG_LEN);
        if (!arg_storage) {
                page_slice_destroy(&elf_slice);
                return -LINUX_ENOMEM;
        }

        argc = linux_exec_copy_argv_from_user(
                vs, user_argv, arg_storage, kargv);
        if (argc < 0) {
                alloc->m_free(alloc, arg_storage);
                page_slice_destroy(&elf_slice);
                return argc;
        }

        linux_exec_wait_remote_tlb_quiesce(vs);

        e = vspace_clear_user_mappings(vs, handler, true);
        if (e != REND_SUCCESS) {
                if (e == -E_REND_RC_UNEQUAL) {
                        alloc->m_free(alloc, arg_storage);
                        page_slice_destroy(&elf_slice);
                        return -LINUX_EAGAIN;
                }
                linux_exec_abort_unrecoverable(alloc,
                                               arg_storage,
                                               elf_slice,
                                               "vspace_clear_user_mappings",
                                               e);
        }

        e = load_elf_to_vs(elf_slice, vs, &max_load_end);
        if (e != REND_SUCCESS) {
                linux_exec_abort_unrecoverable(alloc,
                                               arg_storage,
                                               elf_slice,
                                               "load_elf_to_vs",
                                               e);
        }

        vaddr elf_base = linux_page_slice_file_base(elf_slice);

        if (!elf_base) {
                linux_exec_abort_unrecoverable(alloc,
                                               arg_storage,
                                               elf_slice,
                                               "elf entry lookup",
                                               -E_RENDEZVOS);
        }
        entry_addr = ELF64_HEADER(elf_base)->e_entry;

        page_slice_destroy(&elf_slice);
        elf_slice = NULL;

        user_sp = generate_user_stack(vs);
        if (!user_sp) {
                linux_exec_abort_unrecoverable(alloc,
                                               arg_storage,
                                               NULL,
                                               "generate_user_stack",
                                               -E_RENDEZVOS);
        }

        initial_stack_sp =
                build_initial_stack(vs, user_sp, argc, kargv, &argv_user);
        alloc->m_free(alloc, arg_storage);
        arg_storage = NULL;
        if (initial_stack_sp == 0) {
                linux_exec_abort_unrecoverable(
                        alloc, NULL, NULL, "build_initial_stack", -E_RENDEZVOS);
        }

        linux_exec_reset_proc_state(current, max_load_end);
        linux_signal_reset_thread_handler_state(
                linux_thread_append(current_thread));

        arch_syscall_set_user_return(syscall_ctx,
                                     &current_thread->ctx,
                                     entry_addr,
                                     initial_stack_sp,
                                     0);
#if defined(_AARCH64_)
        arch_syscall_set_user_int_arg(syscall_ctx, 0, (u64)argc);
        arch_syscall_set_user_int_arg(syscall_ctx, 1, (u64)argv_user);
#endif

        return 0;
}
