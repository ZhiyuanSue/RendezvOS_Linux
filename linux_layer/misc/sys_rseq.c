#include <linux_compat/errno.h>
#include <syscall.h>

i64 sys_rseq(u64 user_rseq, u32 rseq_len, i32 flags, u32 sig)
{
        (void)user_rseq;
        (void)rseq_len;
        (void)flags;
        (void)sig;

        /* glibc disables restartable sequences when the syscall is absent. */
        return -LINUX_ENOSYS;
}
