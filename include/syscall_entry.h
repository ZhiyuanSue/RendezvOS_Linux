#ifndef _SYSCALL_ENTRY_
#define _SYSCALL_ENTRY_
#include <rendezvos/task/tcb.h>

#include <rendezvos/trap.h>

void init_syscall_entry();
void syscall(struct trap_frame* syscall_ctx);

#endif