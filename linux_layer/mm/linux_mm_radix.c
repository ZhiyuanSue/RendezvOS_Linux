/*
 * Linux compat: user VA operations via radix tree + mm_user_utils.
 */
#include <common/align.h>
#include <common/dsa/list.h>
#include <common/mm.h>
#include <linux_compat/linux_mm_radix.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/map_handler.h>
#include <rendezvos/mm/mm_user_utils.h>
#include <rendezvos/mm/pmm.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/mm/vmm_radix_tree.h>
#include <rendezvos/smp/percpu.h>

static bool linux_mm_user_vspace_ok(const VSpace* vs)
{
        return linux_vspace_is_user_table(vs);
}

static vaddr linux_mm_l0_lock_lo(vaddr range_start)
{
        return ROUND_DOWN(range_start, (vaddr)HUGE_PAGE_SIZE);
}

static bool linux_mm_page_is_reserved(VSpace* vs, vaddr va)
{
        struct map_handler* handler = &percpu(Map_Handler);
        ENTRY_FLAGS_t pte_flags = 0;
        int level = 0;
        ppn_t ppn = have_mapped(vs, VPN(va), &pte_flags, &level, handler);

        if (!invalid_ppn(ppn) && (pte_flags & PAGE_ENTRY_VALID))
                return true;

        vaddr page_end;
        ENTRY_FLAGS_t radix_flags = 0;

        if (!vmm_radix_tree_calculate_end_check(va, 1, &page_end))
                return true;
        if (vmm_radix_tree_query_range(
                    vs, va, page_end, &radix_flags, NULL)
            != REND_SUCCESS) {
                return false;
        }
        return (radix_flags & (PAGE_ENTRY_VALID | PAGE_ENTRY_LAZY)) != 0;
}

bool linux_mm_range_is_free(VSpace* vs, vaddr hint, size_t page_num)
{
        vaddr range_end;

        if (!linux_mm_user_vspace_ok(vs) || page_num == 0
            || ROUND_DOWN(hint, PAGE_SIZE) != hint) {
                return false;
        }
        if (!vmm_radix_tree_calculate_end_check(hint, page_num, &range_end))
                return false;

        for (vaddr va = hint; va < range_end; va += PAGE_SIZE) {
                if (linux_mm_page_is_reserved(vs, va))
                        return false;
        }
        return true;
}

static ENTRY_FLAGS_t linux_mm_leaf_pte_flags(ENTRY_FLAGS_t leaf_flags)
{
        ENTRY_FLAGS_t pte = entry_flags_rm_sw_flags(leaf_flags);

        pte &= ~((ENTRY_FLAGS_t)PAGE_ENTRY_COW | PAGE_ENTRY_LAZY
                 | PAGE_ENTRY_REMAP);
        if (!(pte & (ENTRY_FLAGS_t)PAGE_ENTRY_VALID)) {
                pte |= (ENTRY_FLAGS_t)PAGE_ENTRY_VALID;
        }
        if (leaf_flags & (ENTRY_FLAGS_t)PAGE_ENTRY_COW) {
                pte = clear_mask_u64(pte, PAGE_ENTRY_WRITE);
        }
        return pte;
}

error_t linux_mm_reinstall_user_pte(VSpace* vs, vaddr page_va)
{
        struct map_handler* handler = &percpu(Map_Handler);
        vaddr page_end;
        vaddr l0_lo;
        vaddr leaf_va = 0;
        Radix_node_t* leaf = NULL;
        Page* page = NULL;
        ppn_t ppn;
        ENTRY_FLAGS_t pte_flags;
        error_t err = -E_REND_NOFOUND;

        if (!linux_mm_user_vspace_ok(vs)) {
                return -E_IN_PARAM;
        }

        page_va = ROUND_DOWN(page_va, PAGE_SIZE);
        if (!vmm_radix_tree_calculate_end_check(page_va, 1, &page_end)) {
                return -E_IN_PARAM;
        }

        l0_lo = linux_mm_l0_lock_lo(page_va);
        if (vmm_radix_tree_lock_range_big(vs, l0_lo, page_end) != REND_SUCCESS) {
                return -E_RENDEZVOS;
        }

        /* direction 1 = ascending (same as core RADIX_TREE_DIRECTION_INC). */
        leaf = vmm_radix_tree_find_first_occupied_leaf(
                vs, page_va, page_end, 1, &leaf_va);
        if (!leaf || leaf_va != page_va
            || !(leaf->flags & (ENTRY_FLAGS_t)PAGE_ENTRY_VALID)) {
                goto out_unlock;
        }
        if (list_node_is_detached(&leaf->rmap_list)) {
                goto out_unlock;
        }

        page = list_entry(leaf->rmap_list.next, Page, rmap_list);
        ppn = (ppn_t)phy_Page_ppn(page);
        if (invalid_ppn(ppn)) {
                goto out_unlock;
        }

        pte_flags = linux_mm_leaf_pte_flags(leaf->flags);
        err = map(vs, ppn, VPN(page_va), 3, pte_flags, handler);

out_unlock:
        (void)vmm_radix_tree_unlock_range_big(vs, l0_lo, page_end);
        return err;
}

/*
 * User VA <-> kernel buffer via have_mapped + map_handler_copy_data_range.
 *
 * map_handler_copy_data_range already loops on physical page boundaries
 * (src_paddr/dst_paddr += chunk). That assumes physically contiguous pages.
 * Adjacent user VPNs usually map to unrelated PPNs, so we resolve one user
 * page per outer iteration (have_mapped), then let core copy up to the end
 * of that user page (and any physically contiguous tail on the kernel side).
 */
static error_t linux_mm_copy_user_kernel_helper(VSpace* vs, u64 user_va,
                                                void* kbuf, size_t len,
                                                bool to_user)
{
        struct map_handler* handler = &percpu(Map_Handler);
        size_t done = 0;

        if (len == 0) {
                return REND_SUCCESS;
        }
        if (!kbuf || !linux_mm_user_vspace_ok(vs)) {
                return -E_IN_PARAM;
        }

        while (done < len) {
                vaddr uva = (vaddr)user_va + (vaddr)done;
                vaddr page_base = ROUND_DOWN(uva, PAGE_SIZE);
                size_t page_off = (size_t)(uva - page_base);
                size_t chunk = PAGE_SIZE - page_off;
                ENTRY_FLAGS_t pte_flags = 0;
                int level = 0;
                ppn_t uppn;
                paddr user_p;
                paddr kern_p;
                error_t e;

                if (chunk > len - done) {
                        chunk = len - done;
                }

                uppn = have_mapped(vs, VPN(page_base), &pte_flags, &level,
                                   handler);
                if (invalid_ppn(uppn) || !(pte_flags & PAGE_ENTRY_VALID)
                    || level != 3) {
                        return -E_RENDEZVOS;
                }

                user_p = PADDR(uppn) + (paddr)page_off;
                kern_p = KERNEL_VIRT_TO_PHY((vaddr)kbuf + (vaddr)done);

                if (to_user) {
                        e = map_handler_copy_data_range(handler, user_p, kern_p,
                                                        (u64)chunk);
                } else {
                        e = map_handler_copy_data_range(handler, kern_p, user_p,
                                                        (u64)chunk);
                }
                if (e != REND_SUCCESS) {
                        return e;
                }
                done += chunk;
        }
        return REND_SUCCESS;
}

error_t linux_mm_store_to_user(VSpace* vs, u64 user_va, const void* src,
                               size_t len)
{
        if (!src) {
                return -E_IN_PARAM;
        }
        return linux_mm_copy_user_kernel_helper(
                vs, user_va, (void*)(uintptr_t)src, len, true);
}

error_t linux_mm_load_from_user(VSpace* vs, u64 user_va, void* dst, size_t len)
{
        return linux_mm_copy_user_kernel_helper(vs, user_va, dst, len, false);
}

error_t linux_mm_copy_user_range(VSpace* vs, u64 dst_user_va, u64 src_user_va,
                                 size_t len)
{
        struct map_handler* handler = &percpu(Map_Handler);
        u64 done = 0;

        if (len == 0) {
                return REND_SUCCESS;
        }
        if (!linux_mm_user_vspace_ok(vs)) {
                return -E_IN_PARAM;
        }

        while (done < len) {
                vaddr sva = (vaddr)src_user_va + (vaddr)done;
                vaddr dva = (vaddr)dst_user_va + (vaddr)done;
                vaddr sp = ROUND_DOWN(sva, PAGE_SIZE);
                vaddr dp = ROUND_DOWN(dva, PAGE_SIZE);
                u64 so = (u64)(sva - sp);
                u64 doff = (u64)(dva - dp);
                u64 chunk = PAGE_SIZE - (so > doff ? so : doff);
                ENTRY_FLAGS_t sf = 0;
                ENTRY_FLAGS_t df = 0;
                int sl = 0;
                int dl = 0;
                ppn_t sppn;
                ppn_t dppn;
                error_t e;

                if (chunk > len - done) {
                        chunk = len - done;
                }

                sppn = have_mapped(vs, VPN(sp), &sf, &sl, handler);
                dppn = have_mapped(vs, VPN(dp), &df, &dl, handler);
                if (invalid_ppn(sppn) || invalid_ppn(dppn) || sl != 3 || dl != 3) {
                        return -E_RENDEZVOS;
                }

                e = map_handler_copy_data_range(
                        handler, PADDR(dppn) + doff, PADDR(sppn) + so, chunk);
                if (e != REND_SUCCESS) {
                        return e;
                }
                done += chunk;
        }
        return REND_SUCCESS;
}

void* linux_mm_map_user_range(VSpace* vs, vaddr hint, size_t page_num,
                              ENTRY_FLAGS_t flags)
{
        if (!linux_mm_user_vspace_ok(vs) || page_num == 0 || hint == 0
            || ROUND_DOWN(hint, PAGE_SIZE) != hint) {
                return NULL;
        }

        vaddr range_end;
        if (!vmm_radix_tree_calculate_end_check(hint, page_num, &range_end))
                return NULL;

        vaddr l0_lo = linux_mm_l0_lock_lo(hint);
        if (vmm_radix_tree_lock_range_big(vs, l0_lo, range_end) != REND_SUCCESS)
                return NULL;

        vaddr mapped = mm_user_utils_set_range_and_fill(
                vs, hint, page_num, flags);
        (void)vmm_radix_tree_unlock_range_big(vs, l0_lo, range_end);
        return mapped ? (void*)mapped : NULL;
}

void* linux_mm_map_user_range_search(VSpace* vs, vaddr search_start,
                                     size_t page_num, ENTRY_FLAGS_t flags,
                                     int max_probes)
{
        vaddr hint = ROUND_DOWN(search_start, PAGE_SIZE);

        for (int i = 0; i < max_probes; i++) {
                if (linux_mm_range_is_free(vs, hint, page_num)) {
                        void* p =
                                linux_mm_map_user_range(vs, hint, page_num, flags);
                        if (p)
                                return p;
                }
                hint += PAGE_SIZE;
        }
        return NULL;
}

error_t linux_mm_unmap_user_range(VSpace* vs, vaddr start, size_t page_num)
{
        struct map_handler* handler = &percpu(Map_Handler);

        if (!linux_mm_user_vspace_ok(vs) || page_num == 0
            || ROUND_DOWN(start, PAGE_SIZE) != start) {
                return -E_IN_PARAM;
        }

        vaddr range_end;
        if (!vmm_radix_tree_calculate_end_check(start, page_num, &range_end))
                return -E_IN_PARAM;

        int pte_level = 3;
        ENTRY_FLAGS_t pte_flags = 0;
        ppn_t ppn_first =
                have_mapped(vs, VPN(start), &pte_flags, &pte_level, handler);
        if ((i64)ppn_first < 0)
                return (error_t)ppn_first;
        if (invalid_ppn(ppn_first))
                return -E_REND_NOFOUND;

        vaddr l0_lo = linux_mm_l0_lock_lo(start);
        if (vmm_radix_tree_lock_range_big(vs, l0_lo, range_end) != REND_SUCCESS)
                return -E_RENDEZVOS;

        error_t err = mm_user_utils_clean_range_and_unfill(
                vs, start, page_num, ppn_first);
        (void)vmm_radix_tree_unlock_range_big(vs, l0_lo, range_end);
        return err;
}

error_t linux_mm_query_vaddr(VSpace* vs, vaddr va, vaddr* out_start,
                             ENTRY_FLAGS_t* out_flags)
{
        if (!linux_mm_user_vspace_ok(vs) || !out_start || !out_flags)
                return -E_IN_PARAM;

        vaddr page_va = ROUND_DOWN(va, PAGE_SIZE);
        vaddr page_end;
        if (!vmm_radix_tree_calculate_end_check(page_va, 1, &page_end))
                return -E_IN_PARAM;

        error_t err = vmm_radix_tree_query_range(
                vs, page_va, page_end, out_flags, NULL);
        if (err != REND_SUCCESS)
                return err;

        *out_start = page_va;
        return REND_SUCCESS;
}

error_t linux_mm_remap_user_leaf(VSpace* vs, vaddr page_va, ppn_t new_ppn,
                                 ENTRY_FLAGS_t new_flags, ppn_t expect_old_ppn)
{
        if (!linux_mm_user_vspace_ok(vs))
                return -E_IN_PARAM;

        page_va = ROUND_DOWN(page_va, PAGE_SIZE);
        vaddr page_end;
        if (!vmm_radix_tree_calculate_end_check(page_va, 1, &page_end))
                return -E_IN_PARAM;

        vaddr l0_lo = linux_mm_l0_lock_lo(page_va);
        if (vmm_radix_tree_lock_range_big(vs, l0_lo, page_end) != REND_SUCCESS)
                return -E_RENDEZVOS;

        error_t err = mm_user_utils_remap_page(
                vs, page_va, new_ppn, new_flags, expect_old_ppn);
        (void)vmm_radix_tree_unlock_range_big(vs, l0_lo, page_end);
        return err;
}

error_t linux_mm_update_range_flags(VSpace* vs, vaddr start, u64 length_bytes,
                                    ENTRY_FLAGS_t new_flags)
{
        if (!linux_mm_user_vspace_ok(vs) || length_bytes == 0)
                return -E_IN_PARAM;

        start = ROUND_DOWN(start, PAGE_SIZE);
        u64 len_aligned = ROUND_UP(length_bytes, PAGE_SIZE);
        vaddr end_va = start + (vaddr)len_aligned;
        if (end_va <= start)
                return -E_IN_PARAM;

        vaddr l0_lo = linux_mm_l0_lock_lo(start);
        if (vmm_radix_tree_lock_range_big(vs, l0_lo, end_va) != REND_SUCCESS)
                return -E_RENDEZVOS;

        error_t err = REND_SUCCESS;
        vaddr iter = start;

        while (iter < end_va) {
                vaddr sub_start = 0;
                vaddr sub_end = 0;
                ENTRY_FLAGS_t sub_flags = 0;

                if (!vmm_radix_tree_find_first_occupied_interval(vs,
                                                                 iter,
                                                                 end_va,
                                                                 &sub_start,
                                                                 &sub_end,
                                                                 &sub_flags)) {
                        err = -E_IN_PARAM;
                        break;
                }
                if (sub_start > iter) {
                        err = -E_IN_PARAM;
                        break;
                }
                if (sub_end > end_va)
                        sub_end = end_va;
                if (sub_end <= sub_start) {
                        err = -E_IN_PARAM;
                        break;
                }

                err = mm_user_utils_set_range_flags(vs,
                                                    sub_start,
                                                    (u64)(sub_end - sub_start),
                                                    MM_USER_RANGE_FLAGS_ABSOLUTE,
                                                    new_flags,
                                                    0);
                if (err != REND_SUCCESS)
                        break;
                iter = sub_end;
        }

        if (err == REND_SUCCESS && iter < end_va)
                err = -E_IN_PARAM;

        (void)vmm_radix_tree_unlock_range_big(vs, l0_lo, end_va);
        return err;
}
