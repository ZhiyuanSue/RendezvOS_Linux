#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <rendezvos/error.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

typedef struct {
        u64 rlim_cur;
        u64 rlim_max;
} linux_rlimit64_t;

#define LINUX_RLIMIT_STACK  3
#define LINUX_RLIMIT_NOFILE 7

static void linux_rlimit64_default(u32 resource, linux_rlimit64_t *out)
{
        out->rlim_cur = 0;
        out->rlim_max = 0;

        switch (resource) {
        case LINUX_RLIMIT_STACK:
                out->rlim_cur = 8UL * 1024UL * 1024UL;
                out->rlim_max = out->rlim_cur;
                break;
        case LINUX_RLIMIT_NOFILE:
                out->rlim_cur = 256;
                out->rlim_max = 256;
                break;
        default:
                out->rlim_cur = (u64)-1;
                out->rlim_max = (u64)-1;
                break;
        }
}

i64 sys_prlimit64(i32 pid, u32 resource, u64 user_new_rlim, u64 user_old_rlim)
{
        Tcb_Base *task = get_cpu_current_task();
        VSpace *vs;
        linux_rlimit64_t rlim;
        error_t e;

        if (!task || !task->vs) {
                return -LINUX_ESRCH;
        }
        if (pid != 0 && pid != (i32)task->pid) {
                return -LINUX_ESRCH;
        }

        vs = task->vs;
        if (!linux_vspace_is_user_table(vs)) {
                return -LINUX_EFAULT;
        }

        if (user_new_rlim) {
                e = linux_mm_load_from_user(
                        vs, user_new_rlim, &rlim, sizeof(rlim));
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }
                /*
                 * Demo/stub: accept no-op sets for root; reject lowering stack
                 * below current default.
                 */
                if (task->pid != 0 && resource == LINUX_RLIMIT_STACK
                    && rlim.rlim_cur < 64UL * 1024UL) {
                        return -LINUX_EINVAL;
                }
        }

        if (user_old_rlim) {
                linux_rlimit64_default(resource, &rlim);
                e = linux_mm_store_to_user(
                        vs, user_old_rlim, &rlim, sizeof(rlim));
                if (e != REND_SUCCESS) {
                        return -LINUX_EFAULT;
                }
        }

        return 0;
}
