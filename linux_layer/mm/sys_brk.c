#include <common/types.h>
#include <linux_compat/proc_compat.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

u64 sys_brk(u64 new_brk)
{
        /*
         * Minimal stub (P0 wiring): maintain a per-process brk cursor in the
         * linux append area. Mapping/unmapping via nexus comes in Step 2+.
         *
         * NOTE: `pa->brk` must be initialized on process creation (ELF init
         * handler sets `start_brk`/`brk` based on PT_LOAD max end). Otherwise
         * brk(0) may return 0 and start_brk will be recorded incorrectly.
         */
        Tcb_Base* tcb = get_cpu_current_task();
        linux_proc_append_t* pa = linux_proc_append(tcb);
        if (!tcb || !pa)
                return 0;

        if (pa->start_brk == 0)
                pa->start_brk = pa->brk;

        if (new_brk == 0)
                return pa->brk;

        /* Accept the value but do not map pages yet (to be implemented). */
        pa->brk = new_brk;
        return pa->brk;
}
