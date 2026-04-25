#ifndef _RENDEZVOS_LINUX_LAYER_MM_FLAGS_H_
#define _RENDEZVOS_LINUX_LAYER_MM_FLAGS_H_

#include <common/types.h>
#include <rendezvos/mm/vmm.h>

/* Linux prot flags subset. */
#define LINUX_PROT_READ  0x1
#define LINUX_PROT_WRITE 0x2
#define LINUX_PROT_EXEC  0x4

static inline ENTRY_FLAGS_t linux_prot_to_page_flags(i64 prot)
{
        ENTRY_FLAGS_t f = PAGE_ENTRY_USER | PAGE_ENTRY_VALID;
        if (prot & LINUX_PROT_READ)
                f |= PAGE_ENTRY_READ;
        if (prot & LINUX_PROT_WRITE)
                f |= PAGE_ENTRY_WRITE;
        if (prot & LINUX_PROT_EXEC)
                f |= PAGE_ENTRY_EXEC;
        return f;
}

#endif
