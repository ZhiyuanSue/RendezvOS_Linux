#include <common/align.h>
#include <common/types.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/elf_init.h>
#include <modules/log/log.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/initcall.h>

/* Global ELF init handler pointer - accessed via extern to avoid GOT issues */
elf_init_handler_t linux_elf_init_handler_ptr = linux_elf_init_handler;

/* Initialize Linux compat per-task state for ELF user programs (e.g. brk). */
void *linux_elf_init_handler(Arch_Task_Context *ctx,
                             const elf_load_info_t *info)
{
        (void)ctx;

        if (!info) {
                pr_emer("[LINUX_ELF_INIT] ERROR: info is NULL!\n");
                return NULL;
        }

        Tcb_Base *tcb = get_cpu_current_task();
        linux_proc_append_t *pa = linux_proc_append(tcb);
        if (!tcb || !pa) {
                pr_emer("[LINUX_ELF_INIT] ERROR: tcb or pa is NULL!\n");
                return NULL;
        }

        u64 brk0 = (u64)info->max_load_end;
        if (brk0 == 0 || brk0 >= KERNEL_VIRT_OFFSET) {
                pr_warn("[LINUX_ELF_INIT] Invalid brk: max_load_end=%lx, using default 0x40000000\n",
                       (u64)info->max_load_end);
                brk0 = 0x40000000;
        }

        pa->brk = brk0;
        pa->start_brk = brk0;
        pr_info("[LINUX_ELF_INIT] Set brk to %lx (max_load_end=%lx)\n",
                brk0, (u64)info->max_load_end);

        return NULL;
}

/* Init function using RendezvOS initcall mechanism */
static void linux_elf_init_initcall(void)
{
        pr_info("[LINUX_ELF_INIT] Module initialized\n");
}
DEFINE_INIT(linux_elf_init_initcall);
