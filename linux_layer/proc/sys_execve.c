#include <common/string.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc/linux_exec.h>
#include <linux_compat/proc_compat.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/trap/trap.h>
#include <syscall.h>

#if defined(_X86_64_)
#include <arch/x86_64/tcb_arch.h>
#elif defined(_AARCH64_)
#include <arch/aarch64/tcb_arch.h>
#endif

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
                char *dst = arg_storage + (size_t)argc * LINUX_EXEC_MAX_ARG_LEN;

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
                                            LINUX_EXEC_MAX_ARG_LEN);
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }
                dst[LINUX_EXEC_MAX_ARG_LEN - 1] = '\0';
                if (!exec_arg_string_valid(dst, LINUX_EXEC_MAX_ARG_LEN)) {
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

i64 sys_execve(struct trap_frame *syscall_ctx, u64 user_filename, u64 user_argv,
               u64 user_envp)
{
        (void)user_envp;

        Tcb_Base *current = get_cpu_current_task();
        Thread_Base *current_thread = get_cpu_current_thread();
        VSpace *vs;
        struct allocator *alloc = percpu(kallocator);
        char filename[LINUX_EXEC_MAX_PATH];
        char *arg_storage = NULL;
        const char *kargv[LINUX_EXEC_MAX_ARGS + 1];
        error_t e;
        i64 argc;
        i64 ret;
        vaddr entry_addr = 0;
        vaddr initial_stack_sp = 0;

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

        e = linux_mm_load_cstring_from_user(
                vs, user_filename, filename, sizeof(filename));
        if (e != REND_SUCCESS) {
                return (e == -E_IN_PARAM) ? -LINUX_EINVAL : -LINUX_EFAULT;
        }

        arg_storage = alloc->m_alloc(
                alloc, (size_t)LINUX_EXEC_MAX_ARGS * LINUX_EXEC_MAX_ARG_LEN);
        if (!arg_storage) {
                return -LINUX_ENOMEM;
        }

        argc = linux_exec_copy_argv_from_user(
                vs, user_argv, arg_storage, kargv);
        if (argc < 0) {
                alloc->m_free(alloc, arg_storage);
                return argc;
        }

        ret = linux_exec_replace_image(current,
                                       current_thread,
                                       filename,
                                       argc,
                                       kargv,
                                       true,
                                       &entry_addr,
                                       &initial_stack_sp);
        alloc->m_free(alloc, arg_storage);
        if (ret != 0) {
                return ret;
        }

        arch_syscall_set_user_return(syscall_ctx,
                                     &current_thread->ctx,
                                     entry_addr,
                                     initial_stack_sp,
                                     0);
        /*
         * ELF entry register hygiene (must match Linux / glibc _start):
         * - aarch64: arch_syscall_set_user_return writes x0=syscall_ret (0).
         * - x86_64: sysret restores rdx from the syscall save area; clear it
         *   so stale envp is not treated as rtld_fini.
         */
#if defined(_X86_64_)
        syscall_ctx->rdx = 0;
#endif

        return 0;
}
