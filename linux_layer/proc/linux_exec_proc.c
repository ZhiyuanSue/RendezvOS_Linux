#include <linux_compat/proc/linux_exec_proc.h>

#include <common/align.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/signal/signal_state.h>
#include <linux_compat/mm/linux_page_slice_file.h>
#include <modules/elf/elf.h>
#include <modules/log/log.h>
#include <rendezvos/mm/page_slice.h>

void linux_proc_set_heap_from_elf_load(Tcb_Base *task, vaddr max_load_end)
{
        linux_proc_append_t *pa = linux_proc_append(task);
        u64 brk0;

        if (!pa) {
                return;
        }

        brk0 = (u64)max_load_end;
        if (brk0 == 0 || brk0 >= KERNEL_VIRT_OFFSET) {
                pr_warn("[linux_exec] invalid brk max_load_end=%lx, using 0x40000000\n",
                        (u64)max_load_end);
                brk0 = 0x40000000;
        }

        pa->brk = brk0;
        pa->start_brk = brk0;
        pa->mmap_hint = ROUND_UP(brk0, PAGE_SIZE) + PAGE_SIZE;
}

void linux_exec_reset_proc_state(Tcb_Base *task, vaddr max_load_end)
{
        linux_proc_append_t *pa = linux_proc_append(task);

        if (!pa) {
                return;
        }

        linux_signal_proc_reset(task);
        linux_proc_set_heap_from_elf_load(task, max_load_end);
        linux_fs_proc_reset(task);
}

bool linux_exec_elf_slice_valid(struct page_slice *slice)
{
        vaddr base;

        if (!slice) {
                return false;
        }

        base = linux_page_slice_file_base(slice);
        if (!base) {
                return false;
        }
        if (!check_elf_header(base)) {
                return false;
        }
        return get_elf_class(base) == ELFCLASS64;
}
