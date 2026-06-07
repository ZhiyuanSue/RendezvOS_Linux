#include <common/mm.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/signal/signal_altstack.h>

bool linux_signal_altstack_region_mapped(VSpace *vs, vaddr base, size_t len)
{
        vaddr end;
        vaddr va;

        if (!vs || !linux_vspace_is_user_table(vs) || len == 0 || base == 0) {
                return false;
        }

        end = base + (vaddr)len;
        if (end < base) {
                return false;
        }

        for (va = ROUND_DOWN(base, PAGE_SIZE); va < end; va += PAGE_SIZE) {
                vaddr page_start;
                ENTRY_FLAGS_t flags;

                if (linux_mm_query_vaddr(vs, va, &page_start, &flags)
                    != REND_SUCCESS) {
                        return false;
                }
        }
        return true;
}
