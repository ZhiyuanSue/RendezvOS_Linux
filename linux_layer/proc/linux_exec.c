#include <linux_compat/proc/linux_exec.h>

#include <common/dsa/list.h>
#include <common/stdbool.h>
#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/fault.h>
#include <linux_compat/fs/linux_exec_image.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/mm/linux_page_slice_file.h>
#include <linux_compat/proc/linux_exec_proc.h>
#include <linux_compat/proc/linux_exec_stack.h>
#include <linux_compat/proc/wait_ipc.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/signal/signal_init.h>
#include <linux_compat/signal/signal_state.h>
#include <linux_compat/signal/signal_types.h>
#include <modules/elf/elf.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/mm/map_handler.h>
#include <rendezvos/mm/page_slice.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/thread.h>
#include <rendezvos/task/thread_loader.h>

static void linux_exec_abort_unrecoverable(struct allocator *alloc,
                                           struct page_slice *elf_slice,
                                           const char *what, error_t e)
{
        if (elf_slice) {
                page_slice_destroy(&elf_slice);
        }
        (void)alloc;
        pr_error("[EXEC] %s failed after commit (e=%d), terminating task\n",
                 what,
                 (int)e);
        linux_fatal_user_fault(128 + SIGKILL);
}

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

error_t linux_user_task_prepare_new(linux_proc_resource_t *task, Thread_Base *thread)
{
        linux_proc_resource_t *pa;

        if (!task || !thread || !(thread->flags & THREAD_FLAG_USER)) {
                return -E_IN_PARAM;
        }

        pa = task;
        if (!pa) {
                return -E_IN_PARAM;
        }

        INIT_LIST_HEAD(&pa->pending_exits);

        if (linux_signal_proc_attach(task) != REND_SUCCESS) {
                return -E_RENDEZVOS;
        }
        if (linux_signal_thread_attach(thread) != REND_SUCCESS) {
                return -E_RENDEZVOS;
        }
        if (linux_fs_proc_attach(task) != REND_SUCCESS) {
                return -E_RENDEZVOS;
        }

        if (register_process(task) != REND_SUCCESS) {
                pr_warn("[linux_exec] register_process failed pid=%d\n",
                        task->pid);
        }

        {
                Message_Port_t *wait_port =
                        proc_get_or_create_wait_port(task->pid);

                if (!wait_port) {
                        pr_warn("[linux_exec] wait_port create failed pid=%d\n",
                                task->pid);
                } else {
                        ref_put(&wait_port->refcount, free_message_port_ref);
                }
        }

        return REND_SUCCESS;
}

i64 linux_exec_replace_image(linux_proc_resource_t *task, Thread_Base *thread,
                             const char *filename, i64 argc,
                             const char *const kargv[],
                             bool may_abort_after_clear, vaddr *entry_out,
                             vaddr *sp_out)
{
        VSpace *vs;
        struct map_handler *handler = &percpu(Map_Handler);
        struct allocator *alloc = percpu(kallocator);
        struct page_slice *elf_slice = NULL;
        linux_exec_elf_auxv_t elf_auxv;
        error_t e;
        i64 ret;
        vaddr max_load_end = 0;
        vaddr entry_addr;
        vaddr user_sp;
        vaddr initial_stack_sp;
        vaddr elf_base;
        const char *stack_argv[LINUX_EXEC_MAX_ARGS + 1];
        i64 i;

        if (entry_out) {
                *entry_out = 0;
        }
        if (sp_out) {
                *sp_out = 0;
        }

        if (!task || !thread || !filename || argc < 0 || !kargv || !alloc) {
                return -LINUX_EINVAL;
        }
        if (argc > LINUX_EXEC_MAX_ARGS) {
                return -LINUX_E2BIG;
        }

        vs = thread->vs;
        if (!vs || !linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        for (i = 0; i < argc; i++) {
                if (!kargv[i]) {
                        return -LINUX_EINVAL;
                }
                stack_argv[i] = kargv[i];
        }
        stack_argv[argc] = NULL;

        ret = linux_exec_load_elf_slice(vs, filename, alloc, &elf_slice);
        if (ret != 0) {
                return ret;
        }

        if (!linux_exec_elf_slice_valid(elf_slice)) {
                page_slice_destroy(&elf_slice);
                return -LINUX_ENOEXEC;
        }

        linux_exec_wait_remote_tlb_quiesce(vs);

        e = vspace_clear_user_mappings(vs, handler, true);
        if (e != REND_SUCCESS) {
                page_slice_destroy(&elf_slice);
                if (e == -E_REND_RC_UNEQUAL) {
                        return -LINUX_EAGAIN;
                }
                if (may_abort_after_clear) {
                        linux_exec_abort_unrecoverable(
                                alloc,
                                NULL,
                                "vspace_clear_user_mappings",
                                e);
                }
                return -LINUX_EFAULT;
        }

        e = load_elf_to_vs(elf_slice, vs, &max_load_end);
        if (e != REND_SUCCESS) {
                if (may_abort_after_clear) {
                        linux_exec_abort_unrecoverable(
                                alloc, elf_slice, "load_elf_to_vs", e);
                }
                page_slice_destroy(&elf_slice);
                return -LINUX_ENOEXEC;
        }

        elf_base = linux_page_slice_file_base(elf_slice);
        if (!elf_base) {
                if (may_abort_after_clear) {
                        linux_exec_abort_unrecoverable(alloc,
                                                       elf_slice,
                                                       "elf entry lookup",
                                                       -E_RENDEZVOS);
                }
                page_slice_destroy(&elf_slice);
                return -LINUX_ENOEXEC;
        }

        entry_addr = ELF64_HEADER(elf_base)->e_entry;
        if (!linux_exec_elf_auxv_from_kva(elf_base, &elf_auxv)) {
                elf_auxv.have_elf = false;
        }

        page_slice_destroy(&elf_slice);
        elf_slice = NULL;

        user_sp = generate_user_stack(vs);
        if (!user_sp) {
                if (may_abort_after_clear) {
                        linux_exec_abort_unrecoverable(alloc,
                                                       NULL,
                                                       "generate_user_stack",
                                                       -E_RENDEZVOS);
                }
                return -LINUX_ENOMEM;
        }

        initial_stack_sp = linux_exec_build_initial_stack(vs,
                                                          user_sp,
                                                          argc,
                                                          stack_argv,
                                                          filename,
                                                          &elf_auxv,
                                                          NULL);
        if (initial_stack_sp == 0) {
                if (may_abort_after_clear) {
                        linux_exec_abort_unrecoverable(alloc,
                                                       NULL,
                                                       "build_initial_stack",
                                                       -E_RENDEZVOS);
                }
                return -LINUX_EFAULT;
        }

        linux_exec_reset_proc_state(task, max_load_end);
        linux_signal_reset_thread_handler_state(linux_thread_append(thread));

        if (entry_out) {
                *entry_out = entry_addr;
        }
        if (sp_out) {
                *sp_out = initial_stack_sp;
        }
        return 0;
}
