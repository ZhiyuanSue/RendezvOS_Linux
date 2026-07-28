#ifndef _LINUX_COMPAT_PROC_LINUX_EXEC_STACK_H_
#define _LINUX_COMPAT_PROC_LINUX_EXEC_STACK_H_

#include <common/types.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/page_slice.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/task/tcb.h>

#include <rendezvos/task/thread_loader.h>

/* Linux uapi auxv (include/uapi/linux/auxvec.h). */
#define LINUX_AT_NULL     0
#define LINUX_AT_PHDR     3
#define LINUX_AT_PHENT    4
#define LINUX_AT_PHNUM    5
#define LINUX_AT_PAGESZ   6
#define LINUX_AT_ENTRY    9
#define LINUX_AT_UID      11
#define LINUX_AT_EUID     12
#define LINUX_AT_GID      13
#define LINUX_AT_EGID     14
#define LINUX_AT_CLKTCK   17
#define LINUX_AT_SECURE   23
#define LINUX_AT_RANDOM   25

#define LINUX_EXEC_RANDOM_BYTES 16

typedef struct linux_exec_elf_auxv {
        bool have_elf;
        vaddr phdr;
        u64 phent;
        u64 phnum;
        vaddr entry;
} linux_exec_elf_auxv_t;

/*
 * Derive user-visible AT_PHDR / AT_PHNUM / AT_PHENT / AT_ENTRY from a mapped
 * ELF64 image (kernel VA of file header is sufficient before/after PT_LOAD).
 */
bool linux_exec_elf_auxv_from_kva(vaddr elf_kva, linux_exec_elf_auxv_t *out);

bool linux_exec_elf_auxv_from_slice(struct page_slice *slice,
                                    linux_exec_elf_auxv_t *out);

/*
 * Shared by sys_execve (Path A) and Path B gen_task_from_elf bootstrap.
 * Layout: argc, argv[], NULL, envp[], NULL, auxv…, random16, strings.
 * @stack_top is generate_user_stack() return (or Path B adjusted SP).
 * Returns new SP (points at argc) or 0 on failure.
 */
vaddr linux_exec_build_initial_stack(VSpace *vs, vaddr stack_top, i64 argc,
                                     const char *kargv[],
                                     const linux_exec_elf_auxv_t *elf_auxv,
                                     vaddr *argv_user_out);

/*
 * Path B: called from thread.append.init inside run_elf_program (after the
 * new thread is scheduled — NOT during gen_task_from_elf return).
 *
 * For static-glibc ELFs (busybox), rebuilds argc/argv/auxv via
 * linux_exec_build_initial_stack. Musl harness ELFs (one PT_NOTE) skip this.
 *
 * Temporary: argv hardcoded to busybox ash smoke
 *   {"sh","-c","/bin/ls /bin; echo SHELL_OK"}
 * (absolute /bin/ls — empty envp means no PATH). Next: PATH= or script.
 */
error_t linux_exec_bootstrap_elf_spawn_stack(Thread_Base *thread, VSpace *vs,
                                             const elf_load_info_t *info);

#endif /* _LINUX_COMPAT_PROC_LINUX_EXEC_STACK_H_ */
