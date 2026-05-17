#ifndef _LINUX_COMPAT_SIGNAL_INIT_H_
#define _LINUX_COMPAT_SIGNAL_INIT_H_

#include <linux_compat/proc_compat.h>

void linux_signal_init_proc_append(linux_proc_append_t* pa);
void linux_signal_init_thread_append(linux_thread_append_t* ta);

#endif /* _LINUX_COMPAT_SIGNAL_INIT_H_ */
