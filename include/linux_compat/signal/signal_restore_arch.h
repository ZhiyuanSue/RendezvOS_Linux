#ifndef _LINUX_COMPAT_SIGNAL_RESTORE_ARCH_H_
#define _LINUX_COMPAT_SIGNAL_RESTORE_ARCH_H_

#if defined(_AARCH64_)
#include <linux_compat/signal/signal_restore_arch_aarch64.h>
#elif defined(_X86_64_)
#include <linux_compat/signal/signal_restore_arch_x86_64.h>
#else
#error "linux_signal_restore_arch_t: unsupported architecture"
#endif

#endif /* _LINUX_COMPAT_SIGNAL_RESTORE_ARCH_H_ */
