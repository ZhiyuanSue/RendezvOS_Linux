#ifndef _RENDEZVOS_LINUX_COMPAT_ELF_INIT_H_
#define _RENDEZVOS_LINUX_COMPAT_ELF_INIT_H_

#include <common/types.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/thread_loader.h>

/* Initialize linux compat per-task state for ELF user programs (e.g. brk). */
void *linux_elf_init_handler(Arch_Task_Context *ctx, const elf_load_info_t *info);

#endif

