#include <common/types.h>
#include <linux_compat/errno.h>
#include <modules/log/log.h>
#include <rendezvos/task/tcb.h>
#include <syscall.h>

#if defined(_X86_64_)
/*
 * Linux arch_prctl(2) codes (uapi asm-generic).
 * musl x86_64 uses ARCH_SET_FS for thread-local storage (errno, pthread, …).
 */
#define LINUX_ARCH_SET_FS 0x1002
#define LINUX_ARCH_GET_FS 0x1003
#define LINUX_ARCH_SET_GS 0x1004
#define LINUX_ARCH_GET_GS 0x1005

i64 sys_arch_prctl(i32 code, u64 addr)
{
        Thread_Base* thread = get_cpu_current_thread();

        if (!thread) {
                return -LINUX_ESRCH;
        }

        switch (code) {
        case LINUX_ARCH_SET_FS:
                arch_set_user_tls_base(&thread->ctx, addr);
                return 0;
        case LINUX_ARCH_GET_FS:
                return (i64)arch_get_user_tls_base(&thread->ctx);
        case LINUX_ARCH_SET_GS:
                thread->ctx.user_gs = addr;
                /*
                 * user_gs is applied to MSR_KERNEL_GS_BASE on context switch
                 * (see switch_to). Do not wrmsr here: syscall path runs with
                 * swapgs and kernel GS active.
                 */
                return 0;
        case LINUX_ARCH_GET_GS:
                return (i64)thread->ctx.user_gs;
        default:
                pr_debug("[ARCH_PRCTL] unsupported code=%d\n", (int)code);
                return -LINUX_EINVAL;
        }
}

#else

i64 sys_arch_prctl(i32 code, u64 addr)
{
        (void)code;
        (void)addr;
        return -LINUX_ENOSYS;
}

#endif
