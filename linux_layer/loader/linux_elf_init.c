#include <linux_compat/elf_init.h>

#include <common/align.h>
#include <common/types.h>
#include <linux_compat/proc/linux_exec_proc.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/signal/signal_init.h>
#include <linux_compat/initcall.h>
#include <modules/log/log.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/mm/page_slice.h>

extern struct Port_Table *global_port_table;

elf_init_handler_t linux_elf_init_handler_ptr = linux_elf_init_handler;

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

        linux_proc_set_heap_from_elf_load(tcb, info->max_load_end);
        linux_signal_init_proc_append(pa);

        error_t reg_e = register_process(tcb);
        if (reg_e != REND_SUCCESS) {
                pr_warn("[LINUX_ELF_INIT] Failed to register PID=%d: %d\n",
                        tcb->pid,
                        (int)reg_e);
        }

        Message_Port_t *wait_port = proc_get_or_create_wait_port(tcb->pid);
        if (!wait_port) {
                pr_warn("[LINUX_ELF_INIT] Failed to create wait_port for PID=%d\n",
                        tcb->pid);
        } else {
                ref_put(&wait_port->refcount, free_message_port_ref);
        }

        /*
         * Compat policy: file image is copied into user PT_LOAD; drop the
         * staging slice after load. Future page-cache / LRU may retain it.
         */
        if (info->slice) {
                struct page_slice *s = info->slice;

                page_slice_destroy(&s);
        }

        return NULL;
}

static bool linux_elf_init_logged;

static void linux_elf_init_initcall(void)
{
        if (!linux_init_bsp_once(&linux_elf_init_logged))
                return;
        pr_info("[LINUX_ELF_INIT] Module initialized\n");
        linux_init_bsp_mark_done(&linux_elf_init_logged);
}
DEFINE_INIT(linux_elf_init_initcall);
