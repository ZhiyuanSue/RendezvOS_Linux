#ifndef _SYSCALL_ENTRY_
#define _SYSCALL_ENTRY_
#include <arch/x86_64/trap/trap.h>

void init_syscall_entry();
void syscall(struct trap_frame* syscall_ctx);

#endif