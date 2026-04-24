#ifndef _RENDEZVOS_LINUX_COMPAT_ELF_INIT_H_
#define _RENDEZVOS_LINUX_COMPAT_ELF_INIT_H_

#include <common/types.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/task/thread_loader.h>

/* ELF initialization handler type */
typedef void *(*elf_init_handler_t)(Arch_Task_Context *ctx,
                                    const elf_load_info_t *info);

/* Global ELF init handler - set by linux_elf_init module */
extern elf_init_handler_t linux_elf_init_handler_ptr;

/* Initialize linux compat per-task state for ELF user programs (e.g. brk). */
void *linux_elf_init_handler(Arch_Task_Context *ctx,
                             const elf_load_info_t *info);

#endif
