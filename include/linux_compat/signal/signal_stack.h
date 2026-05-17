#ifndef _LINUX_COMPAT_SIGNAL_STACK_H_
#define _LINUX_COMPAT_SIGNAL_STACK_H_

#include <common/types.h>
#include <linux_compat/proc_compat.h>
#include <rendezvos/trap/trap.h>

void signal_set_user_sp(linux_thread_append_t* ta, struct trap_frame* tf,
                        vaddr user_sp);
void signal_restore_user_sp(linux_thread_append_t* ta, struct trap_frame* tf,
                            vaddr user_sp);

#endif /* _LINUX_COMPAT_SIGNAL_STACK_H_ */
