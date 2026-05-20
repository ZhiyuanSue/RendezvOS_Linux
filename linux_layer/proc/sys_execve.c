#include <common/string.h>
#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_types.h>
#include <modules/elf/elf.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/trap/trap.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/mm/map_handler.h>
#include <rendezvos/task/thread_loader.h>
#include <syscall.h>

/* External reference to embedded user payload */
extern u64 _num_app;

/* Maximum path length for execve */
#define EXEC_MAX_PATH 256

/* Maximum number of arguments for execve (Phase 3a) */
#define LINUX_EXEC_MAX_ARGS 128

/*
 * Phase 3a: Simple execve implementation
 *
 * Features:
 * - Single-threaded process only (no de_thread)
 * - Embedded ELF lookup by name
 * - Basic argv support (no envp for now)
 * - Path A return: arch_syscall_set_user_return
 *
 * Limitations:
 * - No shebang support
 * - No envp support
 * - No multi-thread cleanup
 * - No file system (embedded payloads only)
 */

/*
 * Find embedded ELF by name
 *
 * Uses a hardcoded mapping table to match program names to indices.
 * This is necessary because ELF binaries start with ELF headers,
 * not program names.
 *
 * Returns: app index (0-based) on success, -1 on failure
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
                /* Indices must match user_payload/link_app.S (_num_app table). */
                {"test_echo", 41},
                {"test_execve", 17},
                {"test_execve_simple", 50},
                {"test_signal_delivery", 7},
                {"test_phase2b_signal_basic", 8},
                /* Add new programs here as they are added to link_app.S */
        };

        int num_programs = sizeof(program_map) / sizeof(program_map[0]);

        for (int i = 0; i < num_programs; i++) {
                if (strcmp_s(program_map[i].name, filename, EXEC_MAX_PATH) == 0) {
                        if (program_map[i].index < (i64)num_apps) {
                                return program_map[i].index;
                        }
                }
        }

        return -1;
}

/*
 * Validate argv and count arguments
 *
 * Returns: number of arguments on success, negative error code on failure
 */
static i64 validate_and_count_argv(char **user_argv)
{
        Tcb_Base *current = get_cpu_current_task();
        VSpace *vs;
        i64 argc = 0;

        if (!current || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        /* Count arguments */
        if (user_argv != NULL) {
                for (i64 i = 0; i < LINUX_EXEC_MAX_ARGS; i++) {
                        char *arg_ptr;

                        error_t e = linux_mm_load_from_user(
                                vs, (vaddr)&user_argv[i], &arg_ptr,
                                sizeof(arg_ptr));
                        if (e != REND_SUCCESS) {
                                return -LINUX_EFAULT;
                        }

                        if (arg_ptr == NULL) {
                                break;  /* End of argv */
                        }

                        argc++;
                }
        }

        if (argc < 0 || argc > LINUX_EXEC_MAX_ARGS) {
                return -LINUX_EINVAL;
        }

        return argc;
}

/*
 * Build initial user stack with argv (direct from user space)
 *
 * This is a simplified version that:
 * - Copies argv strings from user space to user stack
 * - Constructs argv[] pointers and argc
 * - No envp for now (Phase 3a)
 */
static vaddr build_initial_stack(vaddr stack_top, char **user_argv, i64 argc,
                               VSpace *vs)
{
        vaddr current_sp = stack_top;

        /*
         * Linux initial stack layout (simplified):
         * [high addresses]
         * argv strings (growing up) - copied from user space
         * NULL
         * argv[] pointers (growing up)
         * argc (growing up)
         * [low addresses - stack grows down]
         */

        /* First, calculate total size needed for argv strings */
        i64 total_strings_size = 0;
        for (i64 i = 0; i < argc; i++) {
                char *arg_ptr;
                error_t e = linux_mm_load_from_user(
                        vs, (vaddr)&user_argv[i], &arg_ptr, sizeof(arg_ptr));
                if (e != REND_SUCCESS) {
                        return 0;  /* Error */
                }

                if (arg_ptr != NULL) {
                        i64 len = strlen(arg_ptr) + 1;  /* +1 for null terminator */
                        total_strings_size += len;
                }
        }

        /* Reserve space for argv strings */
        current_sp -= total_strings_size;
        current_sp = current_sp & ~0xF;  /* 16-byte align */

        vaddr argv_strings_start = current_sp;

        /* Copy argv strings from user space */
        vaddr string_ptr = argv_strings_start;
        for (i64 i = 0; i < argc; i++) {
                char *arg_ptr;
                error_t e = linux_mm_load_from_user(
                        vs, (vaddr)&user_argv[i], &arg_ptr, sizeof(arg_ptr));
                if (e != REND_SUCCESS) {
                        return 0;  /* Error */
                }

                if (arg_ptr != NULL) {
                        i64 len = strlen(arg_ptr) + 1;
                        /* Copy string from user space to user stack */
                        for (i64 j = 0; j < len; j++) {
                                error_t fe = linux_mm_store_to_user(
                                        vs, string_ptr + j, &arg_ptr[j], 1);
                                if (fe != REND_SUCCESS) {
                                        return 0;  /* Error */
                                }
                        }
                        string_ptr += len;
                }
        }

        /* Reserve space for argv[] pointers and argc */
        current_sp -= (argc + 2) * sizeof(u64);  /* argv[] + NULL + argc */
        current_sp = current_sp & ~0xF;  /* 16-byte align */

        /* Set up argv[] pointers */
        vaddr argv_ptr_area = current_sp;
        string_ptr = argv_strings_start;
        for (i64 i = 0; i < argc; i++) {
                ((u64 *)argv_ptr_area)[i] = string_ptr;
                /* Find string length to advance pointer */
                char *arg_ptr;
                linux_mm_load_from_user(
                        vs, (vaddr)&user_argv[i], &arg_ptr, sizeof(arg_ptr));
                if (arg_ptr != NULL) {
                        i64 len = strlen(arg_ptr) + 1;
                        string_ptr += len;
                }
        }
        ((u64 *)argv_ptr_area)[argc] = 0;  /* argv[argc] = NULL */

        /* Set up argc */
        current_sp -= sizeof(u64);
        ((u64 *)current_sp)[0] = (u64)argc;

        return current_sp;
}

/*
 * Reset process state for execve
 *
 * This resets brk, mmap_hint, and clears signal state.
 * Based on linux_elf_init_handler logic.
 */
static void linux_exec_reset_proc_state(Tcb_Base *task)
{
        linux_proc_append_t *pa = linux_proc_append(task);
        if (!pa) {
                return;
        }

        /* Reset brk and mmap hints */
        pa->brk = 0;
        pa->start_brk = 0;
        pa->mmap_hint = 0;

        /* Clear signal state */
        sigemptyset(&pa->pending_signals);

        /* Note: We don't call register_process() since PID stays the same */
}

/*
 * Main execve syscall implementation
 */
i64 sys_execve(struct trap_frame *syscall_ctx, u64 user_filename, u64 user_argv, u64 user_envp)
{
        (void)user_envp; /* Phase 3a: envp not supported yet */

        Tcb_Base *current = get_cpu_current_task();
        Thread_Base *current_thread = get_cpu_current_thread();
        VSpace *vs;
        char filename[EXEC_MAX_PATH];
        error_t e;
        i64 app_index;
        vaddr elf_start, elf_end;
        vaddr entry_addr, max_load_end;
        vaddr user_sp;
        i64 argc;
        vaddr initial_stack_sp;

        if (!current || !current_thread || !current->vs) {
                return -LINUX_ESRCH;
        }

        vs = current->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        /* Step 1: Copy and validate filename */
        e = linux_mm_load_from_user(vs, user_filename, filename,
                                    sizeof(filename));
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        /* Ensure filename is null-terminated */
        filename[EXEC_MAX_PATH - 1] = '\0';

        /* Step 2: Find embedded ELF by name */
        app_index = find_embedded_elf_by_name(filename);
        if (app_index < 0) {
                return -LINUX_ENOENT;
        }

        /* Get ELF start and end addresses */
        u64 *app_start_ptr = (u64 *)((vaddr)(&_num_app) +
                                      (app_index * 2 + 1) * sizeof(u64));
        u64 *app_end_ptr = (u64 *)((vaddr)(&_num_app) +
                                    (app_index * 2 + 2) * sizeof(u64));

        elf_start = *app_start_ptr;
        elf_end = *app_end_ptr;

        /* Step 3: Validate ELF (before clearing mappings) */
        extern bool check_elf_header(vaddr elf_header_ptr);
        if (!check_elf_header(elf_start)) {
                pr_warn("[EXEC] Invalid ELF header\n");
                return -LINUX_ENOEXEC;
        }

        /* Step 4: Clear user mappings (allow_self_use: local tlb_cpu_mask bit OK) */
        e = vspace_clear_user_mappings(vs, &percpu(Map_Handler), true);
        if (e != REND_SUCCESS) {
                pr_error("[EXEC] Failed to clear user mappings: %d\n", (int)e);
                return -LINUX_EIO;
        }

        /* Step 5: Load ELF to VSpace */
        e = load_elf_to_vs(elf_start, elf_end, vs, &max_load_end);
        if (e != REND_SUCCESS) {
                pr_error("[EXEC] Failed to load ELF: %d\n", (int)e);
                return -LINUX_EIO;
        }

        /* Get entry address from ELF header */
        if (get_elf_class(elf_start) != ELFCLASS64) {
                pr_error("[EXEC] Only ELF64 is supported\n");
                return -LINUX_ENOEXEC;
        }

        Elf64_Ehdr *elf_header = (Elf64_Ehdr *)elf_start;
        entry_addr = elf_header->e_entry;

        /* Step 6: Generate user stack */
        user_sp = generate_user_stack(vs);
        if (!user_sp) {
                pr_error("[EXEC] Failed to generate user stack\n");
                return -LINUX_EIO;
        }

        /* Step 7: Validate and count argv */
        argc = validate_and_count_argv((char **)user_argv);
        if (argc < 0) {
                pr_error("[EXEC] Failed to validate argv: %ld\n", argc);
                return argc;
        }

        /* Step 8: Build initial stack */
        initial_stack_sp = build_initial_stack(user_sp, (char **)user_argv, argc, vs);
        if (initial_stack_sp == 0) {
                pr_error("[EXEC] Failed to build initial stack\n");
                return -LINUX_EIO;
        }

        /* Step 9: Reset process state */
        linux_exec_reset_proc_state(current);

        /* Step 10: Set user return state (Path A) */
        arch_syscall_set_user_return(
                syscall_ctx,
                &current_thread->ctx,
                entry_addr,
                initial_stack_sp,
                0);  /* syscall return value = 0 for execve */

        /*
         * Success: execve never returns to the old program.
         * The syscall_entry will skip_syscall_ret_assign for us.
         */
        return 0;
}