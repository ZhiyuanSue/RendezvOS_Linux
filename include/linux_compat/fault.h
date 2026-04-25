#ifndef _LINUX_COMPAT_FAULT_H_
#define _LINUX_COMPAT_FAULT_H_

#include <common/types.h>

/*
 * Terminate current Linux-compat thread/task due to an unrecoverable user fault
 * (e.g. SIGSEGV equivalent). Must not return to user mode.
 */
void linux_fatal_user_fault(i64 exit_code);

#endif
