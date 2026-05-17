#include <common/align.h>
#include <common/types.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/signal/signal_init.h>
#include <linux_compat/elf_init.h>
#include <modules/log/log.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/port.h>

/* External reference to global port table */
extern struct Port_Table *global_port_table;

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

        linux_signal_init_proc_append(pa);
        pa->brk = brk0;
        pa->start_brk = brk0;
        pa->mmap_hint = ROUND_UP(brk0, PAGE_SIZE) + PAGE_SIZE;
        pr_info("[LINUX_ELF_INIT] Set brk to %lx (max_load_end=%lx)\n",
                brk0,
                (u64)info->max_load_end);

        /*
         * Register this process in proc_registry so wait4 can find it.
         * This is needed for parent-child relationship tracking.
         */
        error_t reg_e = register_process(tcb);
        if (reg_e != REND_SUCCESS) {
                pr_warn("[LINUX_ELF_INIT] Failed to register PID=%d: %d\n",
                        tcb->pid,
                        (int)reg_e);
                /* Non-fatal: process still works, but wait4 won't find it */
        } else {
                pr_debug(
                        "[LINUX_ELF_INIT] Registered PID=%d in proc_registry\n",
                        tcb->pid);
        }

        {
                Message_Port_t* wait_port =
                        proc_get_or_create_wait_port(tcb->pid);

                if (!wait_port) {
                        pr_warn("[LINUX_ELF_INIT] Failed to create wait_port for PID=%d\n",
                                tcb->pid);
                } else {
                        ref_put(&wait_port->refcount, free_message_port_ref);
                }
        }

        return NULL;
}

/* Init function using RendezvOS initcall mechanism */
static void linux_elf_init_initcall(void)
{
        pr_info("[LINUX_ELF_INIT] Module initialized\n");
}
DEFINE_INIT(linux_elf_init_initcall);
