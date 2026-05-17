#ifndef _LINUX_COMPAT_SIGNAL_RESTORE_H_
#define _LINUX_COMPAT_SIGNAL_RESTORE_H_

#include <common/stdbool.h>
#include <rendezvos/trap/trap.h>

bool signal_restore_user_context(struct trap_frame* tf);

#endif /* _LINUX_COMPAT_SIGNAL_RESTORE_H_ */
