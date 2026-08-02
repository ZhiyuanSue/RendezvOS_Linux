/*
 * Linux compat: user VA operations via radix tree + mm_user_utils.
 */
#include <common/align.h>
#include <common/dsa/list.h>
#include <common/mm.h>
#include <linux_compat/linux_mm_radix.h>
#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/buddy_pmm.h>
#include <rendezvos/mm/map_handler.h>
#include <rendezvos/mm/mm_user_utils.h>
#include <rendezvos/mm/pmm.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/mm/vmm_radix_tree.h>
#include <rendezvos/smp/percpu.h>
#if defined(_X86_64_)
#include <arch/x86_64/boot/arch_setup.h>
#include <arch/x86_64/tcb_arch.h>
#elif defined(_AARCH64_)
#include <arch/aarch64/boot/arch_setup.h>
#include <arch/aarch64/tcb_arch.h>
#endif

static bool linux_mm_user_vspace_ok(const VSpace* vs)
{
        return linux_vspace_is_user_table(vs);
}

static vaddr linux_mm_l0_lock_lo(vaddr range_start)
{
        return ROUND_DOWN(range_start, (vaddr)HUGE_PAGE_SIZE);
}

/*
 * Buddy pmm_alloc requires one physically contiguous block of size
 * round_up_pow2(n) and n <= 2^BUDDY_MAXORDER. User mappings only need VA
 * contiguity — split large requests into greedy power-of-two chunks.
 */
static size_t linux_mm_buddy_chunk_pages(size_t remaining)
{
        size_t chunk = (size_t)1 << BUDDY_MAXORDER;

        while (chunk > remaining)
                chunk >>= 1;
        return chunk;
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
        if (vmm_radix_tree_query_range(vs, va, page_end, &radix_flags, NULL)
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

/*
 * Resolve Page* from a radix leaf's rmap node. leaf->rmap_list is a *list node*
 * on Page::rmap_list (see radix_leaf_link_rmap); after COW there may be several
 * leaves, so leaf->rmap_list.next is not always the Page head.
 *
 * phy_Page_ppn() returns a physical address (misnamed); callers must PPN().
 */
static Page* linux_mm_page_from_leaf_rmap(Radix_node_t* leaf)
{
        struct list_entry* pos;

        if (!leaf || list_node_is_detached(&leaf->rmap_list))
                return NULL;

        pos = leaf->rmap_list.next;
        while (pos != &leaf->rmap_list) {
                Page* cand = list_entry(pos, Page, rmap_list);
                MemSection* sec = cand->sec;

                if (sec && sec->page_count > 0) {
                        Page* base = &sec->pages[0];
                        if (cand >= base && cand < base + sec->page_count)
                                return cand;
                }
                pos = pos->next;
        }
        return NULL;
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
        ENTRY_FLAGS_t pt_flags = 0;
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
        if (vmm_radix_tree_lock_range_big(vs, l0_lo, page_end)
            != REND_SUCCESS) {
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

        /*
         * Prefer PTE PPN (sync-after-clone: PTE already present). Do not use
         * list_entry(leaf->rmap_list.next, Page, ...) — after COW that next
         * is often another leaf; bogus ppn then fails map as
         * old=0x7db5000 new=0x7db5000000 (PADDR applied to a paddr).
         */
        ppn = have_mapped(vs, VPN(page_va), &pt_flags, NULL, handler);
        if (invalid_ppn(ppn) || !(pt_flags & PAGE_ENTRY_VALID)) {
                page = linux_mm_page_from_leaf_rmap(leaf);
                if (!page)
                        goto out_unlock;
                ppn = (ppn_t)PPN(phy_Page_ppn(page));
                if (invalid_ppn(ppn))
                        goto out_unlock;
        }

        pte_flags = linux_mm_leaf_pte_flags(leaf->flags);
        /* Same PPN, flags-only update (no PAGE_ENTRY_REMAP). */
        err = map(vs, ppn, VPN(page_va), 3, pte_flags, handler);

out_unlock:
        (void)vmm_radix_tree_unlock_range_big(vs, l0_lo, page_end);
        return err;
}

error_t linux_mm_store_to_user(VSpace* vs, u64 user_va, const void* src,
                               size_t len)
{
        if (!src) {
                return -E_IN_PARAM;
        }
        return map_handler_user_kernel_copy(
                vs, user_va, (void*)(uintptr_t)src, len, true);
}

error_t linux_mm_load_from_user(VSpace* vs, u64 user_va, void* dst, size_t len)
{
        return map_handler_user_kernel_copy(vs, user_va, dst, len, false);
}

error_t linux_mm_load_cstring_from_user(VSpace* vs, u64 user_va, char* dst,
                                        size_t cap)
{
        size_t i;

        if (!dst || cap < 2) {
                return -E_IN_PARAM;
        }
        if (!linux_mm_user_vspace_ok(vs)) {
                return -E_IN_PARAM;
        }

        for (i = 0; i < cap - 1; i++) {
                char c;
                error_t e =
                        linux_mm_load_from_user(vs, user_va + (u64)i, &c, 1);

                if (e != REND_SUCCESS) {
                        return e;
                }
                dst[i] = c;
                if (c == '\0') {
                        return REND_SUCCESS;
                }
        }

        dst[cap - 1] = '\0';
        return -E_IN_PARAM;
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
                if (invalid_ppn(sppn) || invalid_ppn(dppn) || sl != 3
                    || dl != 3) {
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
        struct map_handler* handler = &percpu(Map_Handler);
        size_t mapped_pages = 0;

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

        while (mapped_pages < page_num) {
                size_t chunk = linux_mm_buddy_chunk_pages(page_num - mapped_pages);
                vaddr chunk_va = hint + mapped_pages * PAGE_SIZE;

                if (!mm_user_utils_set_range_and_fill(
                            vs, chunk_va, chunk, flags)) {
                        goto out_rollback;
                }
                mapped_pages += chunk;
        }

        (void)vmm_radix_tree_unlock_range_big(vs, l0_lo, range_end);
        return (void*)hint;

out_rollback:
        /*
         * Each chunk is physically contiguous; unmap per chunk so we do not
         * pass a false contiguous ppn_first across chunk boundaries.
         */
        {
                size_t done = 0;

                while (done < mapped_pages) {
                        size_t chunk = linux_mm_buddy_chunk_pages(mapped_pages - done);
                        vaddr chunk_va = hint + done * PAGE_SIZE;
                        ENTRY_FLAGS_t pte_flags = 0;
                        int pte_level = 3;
                        ppn_t ppn = have_mapped(
                                vs, VPN(chunk_va), &pte_flags, &pte_level, handler);

                        if (!invalid_ppn(ppn)) {
                                (void)mm_user_utils_clean_range_and_unfill(
                                        vs, chunk_va, chunk, ppn);
                        }
                        done += chunk;
                }
        }
        (void)vmm_radix_tree_unlock_range_big(vs, l0_lo, range_end);
        return NULL;
}

void* linux_mm_map_user_range_search(VSpace* vs, vaddr search_start,
                                     size_t page_num, ENTRY_FLAGS_t flags,
                                     int max_probes)
{
        vaddr hint = ROUND_DOWN(search_start, PAGE_SIZE);

        for (int i = 0; i < max_probes; i++) {
                if (linux_mm_range_is_free(vs, hint, page_num)) {
                        void* p = linux_mm_map_user_range(
                                vs, hint, page_num, flags);
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
        size_t done = 0;

        if (!linux_mm_user_vspace_ok(vs) || page_num == 0
            || ROUND_DOWN(start, PAGE_SIZE) != start) {
                return -E_IN_PARAM;
        }

        vaddr range_end;
        if (!vmm_radix_tree_calculate_end_check(start, page_num, &range_end))
                return -E_IN_PARAM;

        vaddr l0_lo = linux_mm_l0_lock_lo(start);
        if (vmm_radix_tree_lock_range_big(vs, l0_lo, range_end) != REND_SUCCESS)
                return -E_RENDEZVOS;

        /*
         * Chunked map → non-contiguous PPNs across chunks. leaf_unbind_range
         * requires a contiguous PPN run — coalesce from PTEs, not one shot.
         */
        while (done < page_num) {
                vaddr page_va = start + done * PAGE_SIZE;
                ENTRY_FLAGS_t pte_flags = 0;
                int pte_level = 3;
                ppn_t run_ppn = have_mapped(
                        vs, VPN(page_va), &pte_flags, &pte_level, handler);
                size_t run = 1;
                error_t err;

                if ((i64)run_ppn < 0) {
                        (void)vmm_radix_tree_unlock_range_big(vs, l0_lo, range_end);
                        return (error_t)run_ppn;
                }
                if (invalid_ppn(run_ppn)) {
                        (void)vmm_radix_tree_unlock_range_big(vs, l0_lo, range_end);
                        return -E_REND_NOFOUND;
                }

                while (done + run < page_num) {
                        vaddr next_va = start + (done + run) * PAGE_SIZE;
                        ENTRY_FLAGS_t nf = 0;
                        int nl = 3;
                        ppn_t next_ppn =
                                have_mapped(vs, VPN(next_va), &nf, &nl, handler);

                        if (invalid_ppn(next_ppn)
                            || next_ppn != run_ppn + (ppn_t)run) {
                                break;
                        }
                        run++;
                }

                err = mm_user_utils_clean_range_and_unfill(
                        vs, page_va, run, run_ppn);
                if (err != REND_SUCCESS) {
                        (void)vmm_radix_tree_unlock_range_big(vs, l0_lo, range_end);
                        return err;
                }
                done += run;
        }

        (void)vmm_radix_tree_unlock_range_big(vs, l0_lo, range_end);
        return REND_SUCCESS;
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

error_t linux_mm_cow_split_page(VSpace* vs, vaddr page_va)
{
        struct map_handler* handler = &percpu(Map_Handler);
        struct pmm* pmm;
        ENTRY_FLAGS_t pt_flags = 0;
        ENTRY_FLAGS_t radix_flags = 0;
        ENTRY_FLAGS_t new_flags;
        vaddr nstart = 0;
        ppn_t old_ppn;
        ppn_t new_ppn;
        size_t alloced = 0;
        error_t e;

        if (!linux_mm_user_vspace_ok(vs) || !handler)
                return -E_IN_PARAM;

        pmm = vs->pmm;
        page_va = ROUND_DOWN(page_va, PAGE_SIZE);

        old_ppn = have_mapped(vs, VPN(page_va), &pt_flags, NULL, handler);
        if (invalid_ppn(old_ppn) || !(pt_flags & PAGE_ENTRY_VALID))
                return REND_SUCCESS; /* unmapped: nothing to split */

        if (pt_flags & PAGE_ENTRY_WRITE)
                return REND_SUCCESS; /* already private writable */

        e = linux_mm_query_vaddr(vs, page_va, &nstart, &radix_flags);
        if (e != REND_SUCCESS)
                return e;
        /*
         * Contract after clone_vspace(COW_PREP): PTE is RO, radix keeps
         * write intent (+ PAGE_ENTRY_COW). Split only when radix says
         * writable — never promote a true RO mapping.
         */
        if (!(radix_flags & PAGE_ENTRY_WRITE))
                return -E_IN_PARAM;

        new_ppn = pmm->pmm_alloc(pmm, 1, &alloced);
        if (invalid_ppn(new_ppn) || alloced != 1)
                return -E_RENDEZVOS;

        e = map_handler_copy_page(handler, new_ppn, old_ppn);
        if (e != REND_SUCCESS) {
                pmm->pmm_free(pmm, new_ppn, 1);
                return e;
        }

        /*
         * Radix is the permission truth (MM_AND_COW). Drop COW/LAZY/REMAP,
         * keep USER/READ/EXEC from radix, restore WRITE for the private copy.
         * Do not rebuild flags from PTE alone — PTE was intentionally RO.
         */
        new_flags = entry_flags_rm_sw_flags(radix_flags) | PAGE_ENTRY_VALID
                    | PAGE_ENTRY_WRITE | PAGE_ENTRY_READ;

        e = linux_mm_remap_user_leaf(
                vs, page_va, new_ppn, new_flags, old_ppn);
        if (e != REND_SUCCESS) {
                pmm->pmm_free(pmm, new_ppn, 1);
                return e;
        }
        return REND_SUCCESS;
}

error_t linux_mm_sync_cow_ptes(VSpace* vs)
{
        vaddr iter;
        vaddr end = (vaddr)USER_SPACE_TOP + 1;

        if (!linux_mm_user_vspace_ok(vs))
                return -E_IN_PARAM;

        iter = PAGE_SIZE;
        while (iter < end) {
                vaddr range_start = 0;
                vaddr range_end = 0;
                ENTRY_FLAGS_t range_flags = 0;
                vaddr va;

                if (!vmm_radix_tree_find_first_occupied_interval(
                            vs,
                            iter,
                            end,
                            &range_start,
                            &range_end,
                            &range_flags)) {
                        break;
                }
                if (range_end <= range_start || range_start < iter)
                        break;

                if ((range_flags & PAGE_ENTRY_VALID)
                    && (range_flags & PAGE_ENTRY_COW)) {
                        for (va = range_start; va < range_end;
                             va += PAGE_SIZE) {
                                error_t e = linux_mm_reinstall_user_pte(vs, va);
                                if (e != REND_SUCCESS
                                    && e != -E_REND_NOFOUND) {
                                        return e;
                                }
                        }
                }
                iter = range_end;
        }
        return REND_SUCCESS;
}

void linux_mm_cow_break_user_stack(VSpace* vs, vaddr user_sp)
{
        vaddr page;
        unsigned i;

        if (!linux_mm_user_vspace_ok(vs) || user_sp < PAGE_SIZE)
                return;

        /*
         * Prefer live syscall scratch SP when available (same source
         * arch_ctx_refresh uses inside copy_thread).
         */
#if defined(_X86_64_)
        if (percpu(user_rsp_scratch) >= PAGE_SIZE)
                user_sp = (vaddr)percpu(user_rsp_scratch);
#elif defined(_AARCH64_)
        /* SP_EL0 is refreshed into ctx by arch_ctx_refresh; caller passes ctx. */
#endif

        page = ROUND_DOWN(user_sp, PAGE_SIZE);
        /* A few frames below SP — wait/fork locals often span >1 page. */
        for (i = 0; i < 4; i++) {
                if (page < PAGE_SIZE)
                        break;
                (void)linux_mm_cow_split_page(vs, page);
                page -= PAGE_SIZE;
        }
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

                if (!vmm_radix_tree_find_first_occupied_interval(
                            vs, iter, end_va, &sub_start, &sub_end, &sub_flags)) {
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

                err = mm_user_utils_set_range_flags(
                        vs,
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
