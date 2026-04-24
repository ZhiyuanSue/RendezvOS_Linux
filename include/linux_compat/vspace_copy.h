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
 * This is a Linux wrapper over core's vspace clone primitive.
 * Current policy: user 4K pages only + COW preparation (share ppn, downgrade
 * parent/child writable leaves to read-only).
 */
error_t linux_copy_vspace(VS_Common *parent_vs, VS_Common **child_vs_ptr);

#endif
