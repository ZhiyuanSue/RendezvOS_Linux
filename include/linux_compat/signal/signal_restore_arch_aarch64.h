#ifndef _LINUX_COMPAT_SIGNAL_RESTORE_ARCH_AARCH64_H_
#define _LINUX_COMPAT_SIGNAL_RESTORE_ARCH_AARCH64_H_

#include <common/types.h>

/*
 * AArch64 EL0 syscall trap frame snapshot for rt_sigreturn.
 * eret restores all REGS[] from the live trap frame, not only ELR/SP_EL0/x0.
 */
typedef struct linux_signal_restore_arch {
        u64 regs[31];
        u64 spsr;
} linux_signal_restore_arch_t;

#endif /* _LINUX_COMPAT_SIGNAL_RESTORE_ARCH_AARCH64_H_ */
