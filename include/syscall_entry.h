#ifndef _SYSCALL_ENTRY_
#define _SYSCALL_ENTRY_
#include <common/types.h>
#include <rendezvos/task/tcb.h>

void init_syscall_entry();
void syscall(Arch_Syscall_Context* syscall_ctx);

#endif