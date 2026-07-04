#ifndef _LINUX_COMPAT_TIME_ARCH_H_
#define _LINUX_COMPAT_TIME_ARCH_H_

#include <common/types.h>

/*
 * Arch-specific boot wall-clock snapshot for REALTIME syscalls.
 * Implementations live in linux_layer/time/arch/time_arch_{x86_64,aarch64}.c.
 * Portable time logic lives in linux_layer/time/linux_ktime.c.
 */
u64 linux_time_arch_boot_realtime_us(void);

#endif /* _LINUX_COMPAT_TIME_ARCH_H_ */
