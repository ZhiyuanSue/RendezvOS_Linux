#ifndef _RENDEZVOS_LINUX_COMPAT_VSPACE_COPY_H_
#define _RENDEZVOS_LINUX_COMPAT_VSPACE_COPY_H_

#include <rendezvos/error.h>
#include <rendezvos/mm/vmm.h>

/*
 * Copy a vspace for fork implementation.
 *
 * @param parent_vs: Parent vspace to copy from
 * @param child_vs_ptr: Output pointer for newly created child vspace
 * @return: 0 on success, negative error code on failure
 *
 * Wrapper over core `clone_vspace(COW_PREP)`: share PPN, clear PTE WRITE on
 * parent and child, radix keeps write intent + `PAGE_ENTRY_COW`.
 */
error_t linux_copy_vspace(VSpace *parent_vs, VSpace **child_vs_ptr);

#endif
