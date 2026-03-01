#ifndef _SYSCALL_ENTRY_
#define _SYSCALL_ENTRY_
#include <rendezvos/task/tcb.h>

#include <rendezvos/trap.h>

#ifdef _AARCH64_
#include <modules/RendezvOS_Linux/arch/aarch64/syscall_ids.h>
#elif defined _X86_64_
#include <modules/RendezvOS_Linux/arch/x86_64/syscall_ids.h>
#endif

void init_syscall_entry();
void syscall(struct trap_frame* syscall_ctx);

#endif