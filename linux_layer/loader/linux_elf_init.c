#include <common/align.h>
#include <common/types.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/elf_init.h>
#include <modules/log/log.h>
#include <rendezvos/task/tcb.h>

void *linux_elf_init_handler(Arch_Task_Context *ctx,
                             const elf_load_info_t *info)
{
        (void)ctx;
        if (!info)
                return NULL;

        Tcb_Base *tcb = get_cpu_current_task();
        linux_proc_append_t *pa = linux_proc_append(tcb);
        if (!tcb || !pa)
                return NULL;

        u64 brk0 = (u64)info->max_load_end;
        if (brk0 == 0 || brk0 >= KERNEL_VIRT_OFFSET) {
                pr_warn("[linux_elf_init] bad initial brk (max_load_end=%lx)\n",
                        (u64)info->max_load_end);
                return NULL;
        }

        pa->brk = brk0;
        pa->start_brk = brk0;
        return NULL;
}
