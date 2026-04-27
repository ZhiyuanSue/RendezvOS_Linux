#include <common/align.h>
#include <common/types.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/elf_init.h>
#include <modules/log/log.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/port.h>

/* External reference to global port table */
extern struct Port_Table* global_port_table;

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
                brk0,
                (u64)info->max_load_end);

        /*
         * Register this process in proc_registry so wait4 can find it.
         * This is needed for parent-child relationship tracking.
         */
        error_t reg_e = register_process(tcb);
        if (reg_e != REND_SUCCESS) {
                pr_warn("[LINUX_ELF_INIT] Failed to register PID=%d: %d\n",
                        tcb->pid, (int)reg_e);
                /* Non-fatal: process still works, but wait4 won't find it */
        } else {
                pr_debug("[LINUX_ELF_INIT] Registered PID=%d in proc_registry\n",
                         tcb->pid);
        }

        /*
         * Create wait_port for this process.
         * This ensures the port always exists (no race condition with child exit).
         * Child processes will send exit notifications to this port.
         */
        char port_name[32];
        const char* prefix = "wait_port_";
        pid_t pid = tcb->pid;

        /* Generate port name: "wait_port_<pid>" */
        for (size_t i = 0; i < 10; i++) {
                port_name[i] = prefix[i];
        }
        size_t idx = 10;

        if (pid == 0) {
                if (idx < 31) {
                        port_name[idx++] = '0';
                }
        } else {
                char rev[16];
                size_t rev_len = 0;
                pid_t temp = pid;
                while (temp > 0 && rev_len < sizeof(rev)) {
                        rev[rev_len++] = '0' + (temp % 10);
                        temp /= 10;
                }
                for (size_t i = 0; i < rev_len && idx < 31; i++) {
                        port_name[idx++] = rev[rev_len - 1 - i];
                }
        }
        port_name[idx] = '\0';

        Message_Port_t* wait_port = thread_lookup_port(port_name);
        if (!wait_port) {
                pr_debug("[LINUX_ELF_INIT] Creating wait_port for PID=%d\n", pid);
                wait_port = create_message_port(port_name);
                if (wait_port) {
                        error_t reg_e = register_port(global_port_table, wait_port);
                        if (reg_e != REND_SUCCESS) {
                                pr_warn("[LINUX_ELF_INIT] Failed to register wait_port '%s': %d\n",
                                        port_name, (int)reg_e);
                                delete_message_port_structure(wait_port);
                        } else {
                                pr_debug("[LINUX_ELF_INIT] Created and registered wait_port '%s' for PID=%d\n",
                                         port_name, pid);
                        }
                } else {
                        pr_error("[LINUX_ELF_INIT] Failed to create wait_port for PID=%d\n",
                                 pid);
                }
        } else {
                pr_debug("[LINUX_ELF_INIT] wait_port '%s' already exists for PID=%d\n",
                         port_name, pid);
                ref_put(&wait_port->refcount, free_message_port_ref);
        }

        return NULL;
}

/* Init function using RendezvOS initcall mechanism */
static void linux_elf_init_initcall(void)
{
        pr_info("[LINUX_ELF_INIT] Module initialized\n");
}
DEFINE_INIT(linux_elf_init_initcall);
