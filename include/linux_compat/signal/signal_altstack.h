#ifndef _LINUX_COMPAT_SIGNAL_ALTSTACK_H_
#define _LINUX_COMPAT_SIGNAL_ALTSTACK_H_

#include <common/types.h>
#include <rendezvos/mm/vmm.h>

/** True when every page in [base, base+len) has a radix/PTE mapping. */
bool linux_signal_altstack_region_mapped(VSpace *vs, vaddr base, size_t len);

#endif /* _LINUX_COMPAT_SIGNAL_ALTSTACK_H_ */
