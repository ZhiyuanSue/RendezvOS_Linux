/*
 * Illegal-instruction trap handler for Linux compat.
 *
 * User mode: exit the process (Linux SIGILL-like behaviour).
 * Kernel mode: dump via arch_unknown_trap_handler, request powerd shutdown,
 * then halt — never return to the faulting instruction.
 */

#include <common/types.h>
#include <modules/log/log.h>
#include <rendezvos/system/powerd.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/trap/trap.h>
#include <linux_compat/errno.h>
#include <linux_compat/initcall.h>
#include <syscall.h>
#include <rendezvos/smp/percpu.h>

#if defined(_AARCH64_)
#include <arch/aarch64/sync/barrier.h>
#elif defined(_X86_64_)
#include <arch/x86_64/sync/barrier.h>
#endif

static void linux_trap_kernel_fatal(struct trap_frame *tf, const char *summary)
{
        pr_error("%s\n", summary);
        arch_unknown_trap_handler(tf);
        (void)rendezvos_request_poweroff();
        for (;;) {
                arch_cpu_relax();
        }
}

static void linux_illegal_instr_trap_handler(struct trap_frame *tf)
{
        Tcb_Base *current;
        bool is_user = false;
        u64 trap_id = 0;

#if defined(_AARCH64_)
        struct aarch64_trap_info info;

        arch_populate_trap_info(tf, &info);
        is_user = info.is_user;
        trap_id = TRAP_ID(tf->trap_info);
#elif defined(_X86_64_)
        struct x86_64_trap_info info;

        arch_populate_trap_info(tf, &info);
        is_user = info.is_user;
        trap_id = TRAP_ID(tf->trap_info);
#else
#error "Unsupported architecture"
#endif

        current = get_cpu_current_task();

        if (is_user && current) {
                pr_warn("[TRAP] illegal instruction in user mode pid=%d trap_id=%lu - exiting\n",
                        current->pid,
                        (unsigned long)trap_id);
                sys_exit_group(-LINUX_ENOSYS);
                __builtin_unreachable();
        }

        linux_trap_kernel_fatal(
                tf,
                "[TRAP] illegal instruction in kernel mode - requesting poweroff");
}

static void linux_unknown_class_trap_handler(struct trap_frame *tf)
{
        Tcb_Base *current;
        bool is_user = false;
        u64 trap_id = 0;

#if defined(_AARCH64_)
        struct aarch64_trap_info info;

        arch_populate_trap_info(tf, &info);
        is_user = info.is_user;
        trap_id = TRAP_ID(tf->trap_info);
#elif defined(_X86_64_)
        struct x86_64_trap_info info;

        arch_populate_trap_info(tf, &info);
        is_user = info.is_user;
        trap_id = TRAP_ID(tf->trap_info);
#else
#error "Unsupported architecture"
#endif

        current = get_cpu_current_task();

        if (is_user && current) {
                pr_warn("[TRAP] unhandled trap in user mode pid=%d trap_id=%lu - exiting\n",
                        current->pid,
                        (unsigned long)trap_id);
                sys_exit_group(-LINUX_ENOSYS);
                __builtin_unreachable();
        }

        linux_trap_kernel_fatal(
                tf,
                "[TRAP] unhandled trap in kernel mode - requesting poweroff");
}

void linux_unknown_trap_init(void)
{
        register_fixed_trap(TRAP_CLASS_ILLEGAL_INSTR,
                            linux_illegal_instr_trap_handler,
                            IRQ_NO_ATTR);
        register_fixed_trap(TRAP_CLASS_UNKNOWN,
                            linux_unknown_class_trap_handler,
                            IRQ_NO_ATTR);

        if (linux_init_on_bsp()) {
                pr_info("[TRAP] Registered illegal-instruction and unknown-class handlers\n");
        }
}

DEFINE_INIT_LEVEL(linux_unknown_trap_init, 1);
