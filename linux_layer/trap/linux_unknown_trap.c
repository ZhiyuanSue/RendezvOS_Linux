/*
 * Undefined Instruction Trap Handler for Linux Compat Layer
 *
 * This handler catches undefined instruction exceptions (EC=0x00 on aarch64)
 * to provide Linux-compatible error handling instead of kernel panic.
 *
 * Strategy:
 * - User mode exceptions: gracefully exit the current process
 * - Kernel mode exceptions: treat as kernel bug, panic
 */

#include <common/types.h>
#include <modules/log/log.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/trap/trap.h>
#include <linux_compat/errno.h>
#include <linux_compat/signal/signal_types.h>
#include <linux_compat/proc_compat.h>
#include <syscall.h>
#include <rendezvos/smp/percpu.h>

/* Get trap interface and NR_IRQ from core */
#include <rendezvos/trap/trap.h>

/* Access core layer's percpu trap vector (defined in core/kernel/trap/trap.c) */
extern struct irq irq_vector[NR_IRQ];

/*
 * Undefined instruction trap handler
 *
 * Handles EC=0x00 (unknown reason/undefined instruction) on aarch64
 * which maps to trap_id=0.
 */
static void linux_undefined_instruction_handler(struct trap_frame *tf)
{
        Tcb_Base *current;
        bool is_user = false;

#if defined(_AARCH64_)
        struct aarch64_trap_info info;
        arch_populate_trap_info(tf, &info);
        is_user = info.is_user;
#elif defined(_X86_64_)
        struct x86_64_trap_info info;
        arch_populate_trap_info(tf, &info);
        is_user = info.is_user;
#else
#error "Unsupported architecture"
#endif

        current = get_cpu_current_task();

        /*
         * User mode undefined instruction: gracefully exit the current process
         * instead of panicking the entire kernel.
         */
        if (is_user && current) {
                pr_warn("[UNDEF_INSTR] User mode undefined instruction - exiting process %d\n",
                        current->pid);
                sys_exit_group(-LINUX_ENOSYS);
                __builtin_unreachable();
        }

        /*
         * Kernel mode undefined instruction: this is a kernel bug
         */
        pr_error("[UNDEF_INSTR] Kernel mode undefined instruction - this is a BUG\n");
}

/*
 * Initialize undefined instruction trap handler
 *
 * Check which trap IDs already have handlers, then register our handler
 * only for those without handlers. This provides comprehensive coverage while
 * avoiding overwrite of critical handlers like syscall.
 */
void linux_unknown_trap_init(void)
{
        int registered_count = 0;
        int skipped_count = 0;

        /*
         * Iterate through all trap IDs and register our handler only for those
         * that don't already have a handler.
         *
         * This provides comprehensive coverage while avoiding overwrite of
         * critical handlers like syscall.
         */
        for (int i = 0; i < NR_IRQ; i++) {
                /* Check if this trap ID already has a handler */
                if (percpu(irq_vector[i].irq_handler) == NULL) {
                        register_irq_handler(
                                i,
                                linux_undefined_instruction_handler,
                                IRQ_NO_ATTR
                        );
                        registered_count++;
                } else {
                        skipped_count++;
                }
        }

        pr_info("[UNDEF_INSTR] Registered handler for %d trap IDs, skipped %d already registered\n",
                registered_count, skipped_count);
}
DEFINE_INIT_LEVEL(linux_unknown_trap_init, 1);  /* Run after core init (level 0) */
