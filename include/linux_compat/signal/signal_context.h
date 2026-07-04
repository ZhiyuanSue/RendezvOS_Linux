#ifndef _LINUX_COMPAT_SIGNAL_CONTEXT_H_
#define _LINUX_COMPAT_SIGNAL_CONTEXT_H_

#include <common/types.h>
#include <linux_compat/proc_compat.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/trap/trap.h>

/*
 * Arch-specific save/restore of the user-visible syscall trap frame for
 * rt_sigreturn. Implementations live in linux_layer/signal/arch/.
 */
void linux_signal_arch_save_context(struct trap_frame* tf,
                                    Arch_Task_Context* ctx,
                                    linux_signal_restore_t* rs);

void linux_signal_arch_restore_context(struct trap_frame* tf,
                                       Arch_Task_Context* ctx,
                                       linux_signal_restore_t* rs,
                                       vaddr user_sp);

#endif /* _LINUX_COMPAT_SIGNAL_CONTEXT_H_ */
