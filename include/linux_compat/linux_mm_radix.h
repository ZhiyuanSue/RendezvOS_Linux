#ifndef _LINUX_COMPAT_LINUX_MM_RADIX_H_
#define _LINUX_COMPAT_LINUX_MM_RADIX_H_

#include <common/mm.h>
#include <common/types.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/vmm.h>

/**
 * @file linux_mm_radix.h
 * @brief Linux compat wrappers over radix + mm_user_utils (replaces nexus for
 *        user VA map/unmap/query/mprotect/COW).
 */

/**
 * @brief Per-process user address space (own page tables + radix metadata).
 *
 * Replaces the old nexus-era @c vs_common_is_table_vspace (kernel per-CPU heap
 * @c vs_common is no longer exposed to linux_layer).
 */
static inline bool linux_vspace_is_user_table(const VSpace* vs)
{
        return vs && vs != &root_vspace && vs->pmm && vs->vspace_root_addr
               && vs->root_radix;
}

/** True when no radix/PTE entry covers any page in [@p hint, hint + @p
 * page_num). */
bool linux_mm_range_is_free(VSpace* vs, vaddr hint, size_t page_num);

/** Copy @p len bytes from kernel @p src to user VA @p user_va (mapped pages
 * only). */
error_t linux_mm_store_to_user(VSpace* vs, u64 user_va, const void* src,
                               size_t len);

/** Copy @p len bytes from user VA @p user_va to kernel @p dst (mapped pages
 * only). */
error_t linux_mm_load_from_user(VSpace* vs, u64 user_va, void* dst, size_t len);

/**
 * Copy a NUL-terminated string from user VA (byte-at-a-time; safe near stack
 * top where a bulk @p cap read could cross an unmapped guard page).
 */
error_t linux_mm_load_cstring_from_user(VSpace* vs, u64 user_va, char* dst,
                                        size_t cap);

/** Copy between two user VAs (mapped leaf pages only). */
error_t linux_mm_copy_user_range(VSpace* vs, u64 dst_user_va, u64 src_user_va,
                                 size_t len);

/** Map @p page_num user pages at @p hint (must be page-aligned; 0 = failure).
 */
void* linux_mm_map_user_range(VSpace* vs, vaddr hint, size_t page_num,
                              ENTRY_FLAGS_t flags);

/**
 * @brief Best-effort map at/above @p search_start (page-aligned); probes
 * forward.
 */
void* linux_mm_map_user_range_search(VSpace* vs, vaddr search_start,
                                     size_t page_num, ENTRY_FLAGS_t flags,
                                     int max_probes);

/** Unmap + drop radix reservation + pmm_free contiguous run from @p start. */
error_t linux_mm_unmap_user_range(VSpace* vs, vaddr start, size_t page_num);

/** Query one page: radix shadow flags; @p out_start is the page base. */
error_t linux_mm_query_vaddr(VSpace* vs, vaddr va, vaddr* out_start,
                             ENTRY_FLAGS_t* out_flags);

/** COW / single-page remap (radix + PTE + rmap); expects old PTE mapped. */
error_t linux_mm_remap_user_leaf(VSpace* vs, vaddr page_va, ppn_t new_ppn,
                                 ENTRY_FLAGS_t new_flags, ppn_t expect_old_ppn);

/** mprotect-style: update each uniform occupied sub-interval in [@p start,
 * end). */
error_t linux_mm_update_range_flags(VSpace* vs, vaddr start, u64 length_bytes,
                                    ENTRY_FLAGS_t new_flags);

/**
 * Install a leaf PTE when radix already has VALID (+ rmap) but the page table
 * entry is missing (fork/COW paths; mm_user_utils_fill_page no-ops on VALID).
 */
error_t linux_mm_reinstall_user_pte(VSpace* vs, vaddr page_va);

#endif /* _LINUX_COMPAT_LINUX_MM_RADIX_H_ */
