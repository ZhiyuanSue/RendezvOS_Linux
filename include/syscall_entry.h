#ifndef _SYSCALL_ENTRY_
#define _SYSCALL_ENTRY_
#include <common/types.h>

void init_syscall_entry();
void syscall(i64 syscall_id, ...);

#endif