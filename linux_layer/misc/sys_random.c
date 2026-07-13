#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <rendezvos/error.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

#define LINUX_GETRANDOM_MAX_BYTES 256U

static void linux_fill_pseudo_random(u8 *buf, size_t len, u64 mix)
{
        for (size_t i = 0; i < len; i++) {
                buf[i] = (u8)(((mix >> ((i & 7U) * 8U)) ^ (0xA5U + i)) & 0xFFU);
        }
}

i64 sys_getrandom(u64 user_buf, u64 count, u32 flags)
{
        Tcb_Base *task = get_cpu_current_task();
        VSpace *vs;
        u8 stack_buf[LINUX_GETRANDOM_MAX_BYTES];
        u64 mix;
        error_t e;

        (void)flags;

        if (!task || !task->vs) {
                return -LINUX_ESRCH;
        }
        if (count == 0) {
                return 0;
        }
        if (!user_buf) {
                return -LINUX_EFAULT;
        }

        vs = task->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        if (count > LINUX_GETRANDOM_MAX_BYTES) {
                count = LINUX_GETRANDOM_MAX_BYTES;
        }

        mix = (u64)task->pid << 32;
        mix ^= (u64)percpu(cpu_number) << 16;
        mix ^= (u64)(uintptr_t)user_buf;
        linux_fill_pseudo_random(stack_buf, (size_t)count, mix);

        e = linux_mm_store_to_user(vs, user_buf, stack_buf, (size_t)count);
        if (e != REND_SUCCESS) {
                return -LINUX_EFAULT;
        }

        return (i64)count;
}
