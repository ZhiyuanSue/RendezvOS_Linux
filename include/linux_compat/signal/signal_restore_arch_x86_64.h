#ifndef _LINUX_COMPAT_SIGNAL_RESTORE_ARCH_X86_64_H_
#define _LINUX_COMPAT_SIGNAL_RESTORE_ARCH_X86_64_H_

#include <common/types.h>

/*
 * x86_64 syscall stack frame (arch_exit_kernel / sysret) restores these
 * registers from the trap frame; rsp comes from user_rsp_scratch.
 */
typedef struct linux_signal_restore_arch {
        u64 r15;
        u64 r14;
        u64 r13;
        u64 r12;
        u64 rbp;
        u64 rbx;
        u64 r11;
        u64 r10;
        u64 r9;
        u64 r8;
        u64 rdx;
        u64 rsi;
        u64 rdi;
} linux_signal_restore_arch_t;

#endif /* _LINUX_COMPAT_SIGNAL_RESTORE_ARCH_X86_64_H_ */
