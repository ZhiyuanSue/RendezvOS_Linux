#ifndef _SYSCALL_ENTRY_
#define _SYSCALL_ENTRY_
#include <rendezvos/task/tcb.h>

#include <rendezvos/trap.h>

#ifdef _AARCH64_
#include <arch/aarch64/syscall_ids.h>
#elif defined _X86_64_
#include <arch/x86_64/syscall_ids.h>
#else
#include <arch/x86_64/syscall_ids.h>
#endif

void init_syscall_entry();
void syscall(struct trap_frame *syscall_ctx);

#endif