#include <common/types.h>
#include <common/align.h>
#include <common/mm.h>
#include <common/string.h>
#include <modules/log/log.h>
#include <rendezvos/trap/trap.h>
#include <rendezvos/system/panic.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/mm/map_handler.h>
#include <rendezvos/mm/nexus.h>
#include <rendezvos/mm/pmm.h>
#include <rendezvos/smp/percpu.h>
#include <linux_compat/fault.h>
#include <linux_compat/errno.h>

#if defined(_X86_64_)
#include <arch/x86_64/mm/pmm.h>
#elif defined(_AARCH64_)
#include <arch/aarch64/mm/pmm.h>
#else
#error "Unsupported architecture"
#endif

/*
 * Linux-layer page fault entry.
 *
 * We register a page fault handler using core's architecture-independent
 * trap_class interface. This keeps linux_layer code portable across
 * architectures.
 *
 * The handler can resolve:
 * - lazy anonymous allocation (range exists but not filled)
 * - COW write faults
 *
 * Note: We use register_fixed_trap() with TRAP_CLASS_PAGE_FAULT instead of
 * architecture-specific trap IDs. This maps to:
 * - x86_64: #PF (vector 14)
 * - aarch64: data abort (EC 0x24) and instruction abort (EC 0x20)
 */

/*
 * COW page split handler:
 *
 * When a forked process tries to write to a shared page, we need to:
 * 1. Query the current mapping to get the old PPN and flags
 * 2. Verify this is a COW scenario (page present but not writable)
 * 3. Allocate a new physical page
 * 4. Copy the content from the shared page to the new page
 * 5. Use nexus_remap_user_leaf() to update the mapping (which handles:
 *    - Page table remap with WRITE permission
 *    - Nexus tree update
 *    - Rmap (reverse mapping) maintenance
 *    - Old page reference count decrement)
 *
 * This is the core mechanism that makes fork() efficient with COW.
 *
 * We use core's nexus_remap_user_leaf() API which encapsulates all the
 * complex interactions between page tables, nexus tree, and reference counting.
 */
static error_t linux_handle_cow_fault(vaddr fault_addr, bool is_write,
                                      bool is_present)
{
        VS_Common *vs = percpu(current_vspace);
        struct map_handler *handler = &percpu(Map_Handler);
        struct pmm *pmm = vs->pmm;

        if (!vs || !handler || !pmm) {
                pr_error("[COW] NULL vs/handler/pmm\n");
                return -E_RENDEZVOS;
        }

        /* Step 1: Query current mapping */
        ENTRY_FLAGS_t old_flags;
        int entry_level;
        ppn_t old_ppn = have_mapped(
                vs, VPN(fault_addr), &old_flags, &entry_level, handler);

        if (old_ppn <= 0 || !(old_flags & PAGE_ENTRY_VALID)) {
                /* Page not present - not a COW fault */
                return -E_RENDEZVOS;
        }

        /* Step 2: Verify COW condition
         *
         * COW semantics after the fix:
         * - Nexus node: stores original permissions (e.g., RW)
         * - Page table: read-only (COW protection)
         *
         * When a write fault occurs on a COW page:
         * - old_flags (from page table): read-only
         * - But the page should be writable according to nexus
         *
         * We can't directly check nexus flags here without a query interface,
         * so we rely on the fact that:
         * - If page is mapped and read-only, it might be COW
         * - nexus_remap_user_leaf will verify and return error if not COW
         */
        if (old_flags & PAGE_ENTRY_WRITE) {
                /* Page already writable - not a COW fault */
                pr_error("[COW] Page already writable at vaddr=0x%lx\n",
                         fault_addr);
                return -E_RENDEZVOS;
        }

        if (!is_write || !is_present) {
                /* Not a write fault on present page */
                return -E_RENDEZVOS;
        }

        /*
         * At this point, we have a write fault on a present, read-only page.
         * This could be:
         * 1. A COW page (nexus says writable, page table says read-only)
         * 2. A true read-only page (both nexus and page table say read-only)
         *
         * We attempt COW split; nexus_remap_user_leaf will distinguish
         * between these cases and return appropriate error codes.
         */

        pr_debug("[COW] Splitting page at vaddr=0x%lx (old_ppn=0x%lx)\n",
                 fault_addr,
                 (u64)old_ppn);

        /* Step 3: Allocate new physical page */
        size_t alloced_page_number;
        ppn_t new_ppn = pmm->pmm_alloc(pmm, 1, &alloced_page_number);
        if (invalid_ppn(new_ppn) || alloced_page_number != 1) {
                pr_error("[COW] Failed to allocate new physical page\n");
                return -E_RENDEZVOS;
        }

        /* Step 4: Copy page content using map_handler_copy_page() */
        error_t e = map_handler_copy_page(handler, new_ppn, old_ppn);
        if (e != REND_SUCCESS) {
                pr_error("[COW] Failed to copy page content (e=%d)\n", (int)e);
                pmm->pmm_free(pmm, new_ppn, 1);
                return e;
        }

        /* Step 5: Use nexus_remap_user_leaf() to update the mapping
         *
         * This function handles:
         * - Page table remap with WRITE permission
         * - Nexus tree update
         * - Rmap (reverse mapping) maintenance
         * - Old page reference count decrement (via pmm_free)
         * - New page reference count is already set by pmm_alloc
         */
        /* Check if nexus_node is properly initialized */
        if (!vs->_vspace_node) {
                pr_error("[COW] vs->_vspace_node is NULL! va=0x%lx\n",
                         fault_addr);
                pr_error("[COW] This means child vspace has no nexus tree!\n");
                pmm->pmm_free(pmm, new_ppn, 1);
                return -E_RENDEZVOS;
        }

        struct nexus_node *vspace_node = (struct nexus_node *)vs->_vspace_node;
        if (vspace_node->vs_common != vs) {
                pr_error("[COW] nexus_node->vs_common mismatch! va=0x%lx\n",
                         fault_addr);
                pmm->pmm_free(pmm, new_ppn, 1);
                return -E_RENDEZVOS;
        }

        pr_debug("[COW] vspace_node: addr=%p, vs_common=%p, expected vs=%p\n",
                 vspace_node,
                 vspace_node->vs_common,
                 vs);

        /*
         * Step 5: Use nexus_remap_user_leaf() to update the mapping
         *
         * This function now automatically handles page alignment, so we can
         * pass the fault_addr directly without manual alignment.
         */
        pr_debug(
                "[COW] Calling nexus_remap_user_leaf: va=0x%lx, new_ppn=0x%lx, old_ppn=0x%lx, flags=0x%lx\n",
                fault_addr,
                (u64)new_ppn,
                (u64)old_ppn,
                (u64)(old_flags | PAGE_ENTRY_WRITE));

        ENTRY_FLAGS_t new_flags = old_flags | PAGE_ENTRY_WRITE;
        e = nexus_remap_user_leaf(vs, fault_addr, new_ppn, new_flags, old_ppn);

        if (e != REND_SUCCESS) {
                pr_error("[COW] Failed to remap page (e=%d)\n", (int)e);
                pr_error(
                        "[COW] Details: va=0x%lx, new_ppn=0x%lx, old_ppn=0x%lx, flags=0x%lx, entry_level=%d\n",
                        fault_addr,
                        (u64)new_ppn,
                        (u64)old_ppn,
                        (u64)new_flags,
                        entry_level);
                pmm->pmm_free(pmm, new_ppn, 1);
                return e;
        }

        pr_debug(
                "[COW] Successfully split page at vaddr=0x%lx (old_ppn=0x%lx -> new_ppn=0x%lx)\n",
                fault_addr,
                (u64)old_ppn,
                (u64)new_ppn);

        return REND_SUCCESS;
}

static void linux_trap_pf_handler(struct trap_frame *tf)
{
        vaddr fault_addr = arch_get_fault_addr(tf);
        vaddr aligned = ROUND_DOWN(fault_addr, PAGE_SIZE);

        /* Get trap-specific information */
        bool is_write = false;
        bool is_present = false;
        bool is_execute = false;

#if defined(_AARCH64_)
        struct aarch64_trap_info info;
        arch_populate_trap_info(tf, &info);
        is_write = info.is_write;
        is_present = info.is_present;
        is_execute = info.is_execute;
#elif defined(_X86_64_)
        struct x86_64_trap_info info;
        arch_populate_trap_info(tf, &info);
        is_write = info.is_write;
        is_present = info.is_present;
        is_execute = info.is_execute;
#else
#error "Unsupported architecture"
#endif

        pr_debug(
                "[Linux compat] Page fault at vaddr=0x%lx (write=%d, present=%d, exec=%d)\n",
                fault_addr,
                is_write,
                is_present,
                is_execute);

        /*
         * First, determine if this is a kernel or user fault.
         * This must be done early for proper error handling.
         */
        bool is_kernel = arch_int_from_kernel(tf);

        /*
         * Extra diagnostics: query current mapping and nexus.
         *
         * Arch-present bits can be subtle (perm vs translation faults); always
         * consult the page table and nexus view to understand the real state.
         */
        VS_Common *vs = percpu(current_vspace);
        struct map_handler *handler = &percpu(Map_Handler);

        if (!vs || !handler) {
                pr_error("[Linux compat] NULL vs or handler\n");
                goto fatal_fault;
        }

        ENTRY_FLAGS_t pt_flags = 0;
        int level = 0;
        ppn_t ppn = have_mapped(vs, VPN(aligned), &pt_flags, &level, handler);

        pr_debug(
                "[Linux compat] have_mapped: va=0x%lx ppn=0x%lx flags=0x%lx level=%d vs=%p asid=%lu root=0x%lx\n",
                aligned,
                (u64)ppn,
                (u64)pt_flags,
                level,
                (void *)vs,
                (u64)vs->asid,
                (u64)vs->vspace_root_addr);

        vaddr nstart = 0;
        ENTRY_FLAGS_t nflags = 0;
        error_t ne = nexus_query_vaddr(vs, aligned, &nstart, &nflags);

        if (invalid_ppn(ppn)) {
                if (ne == REND_SUCCESS) {
                        pr_debug(
                                "[Linux compat] nexus query: start=0x%lx flags=0x%lx\n",
                                (u64)nstart,
                                (u64)nflags);
                } else {
                        pr_debug(
                                "[Linux compat] nexus query: not found (e=%d)\n",
                                (int)ne);
                }
        }

        /*
         * Page fault handling decision tree based on nexus flags.
         *
         * We classify faults by what nexus says the mapping SHOULD be:
         * - If nexus says writable -> this is a write access
         * - If nexus says executable -> this is an execute access
         * - Otherwise -> this is a read access
         *
         * User-mode faults: SIGSEGV (linux_fatal_user_fault)
         * Kernel-mode faults: kernel_panic
         */

        /* Determine access type from nexus flags */
        bool nexus_is_write = (nflags & PAGE_ENTRY_WRITE);
        bool nexus_is_exec = (nflags & PAGE_ENTRY_EXEC);

        bool page_mapped = !invalid_ppn(ppn) && (pt_flags & PAGE_ENTRY_VALID);
        bool in_nexus = (ne == REND_SUCCESS);

        /*
         * Category 1: NULL pointer detection.
         * Any fault in the first page is almost certainly a bug.
         */
        if (fault_addr < PAGE_SIZE) {
                pr_error("[Linux compat] NULL pointer access at 0x%lx (is_kernel=%d)\n",
                         fault_addr, is_kernel);
#if defined(_X86_64_)
                pr_error("[Linux compat] RIP=0x%lx, RSP=0x%lx, RAX=0x%lx\n",
                         tf->rip, tf->rsp, tf->rax);
#elif defined(_AARCH64_)
                pr_error("[Linux compat] PC=0x%lx, SP=0x%lx\n",
                         tf->ELR, tf->SP);
#endif
                if (is_kernel) {
                        kernel_panic("NULL pointer dereference in kernel\n");
                } else {
                        linux_fatal_user_fault(-LINUX_EFAULT);
                }
                return;
        }

        /*
         * Category 2: Lazy allocation.
         * Page not mapped, but exists in nexus with appropriate permissions.
         *
         * This is the core of demand paging: allocate physical pages on first
         * access rather than pre-allocating. This saves memory and allows
         * sparse address spaces.
         */
        if (!page_mapped && in_nexus) {
                pr_debug(
                        "[Linux compat] Lazy allocation at 0x%lx (nexus flags=0x%lx)\n",
                        fault_addr,
                        (u64)nflags);

                /* Check permissions based on nexus flags */
                if (nexus_is_write && !(nflags & PAGE_ENTRY_WRITE)) {
                        /* Write to RO lazy mapping - should not happen */
                        pr_error(
                                "[Linux compat] Nexus inconsistency: write fault on RO nexus mapping at 0x%lx\n",
                                fault_addr);
                        goto handle_read_only_violation;
                }
                if (nexus_is_exec && !(nflags & PAGE_ENTRY_EXEC)) {
                        /* Execute from NX lazy mapping - permission violation
                         */
                        pr_error(
                                "[Linux compat] Nexus inconsistency: exec fault on NX nexus mapping at 0x%lx\n",
                                fault_addr);
                        goto handle_read_only_violation;
                }

                error_t e;

                /*
                 * Step 1: Allocate physical page
                 */
                struct pmm *pmm = vs->pmm;
                if (!pmm) {
                        pr_error("[Linux compat] PMM is NULL!\n");
                        goto fatal_fault;
                }

                size_t alloced_page_number;
                ppn_t new_ppn = pmm->pmm_alloc(pmm, 1, &alloced_page_number);
                if (invalid_ppn(new_ppn) || alloced_page_number != 1) {
                        pr_error(
                                "[Linux compat] Failed to allocate physical page for lazy allocation\n");
                        goto unhandled_fault;
                }

                pr_debug("[Linux compat] Allocated ppn=0x%lx for va=0x%lx\n",
                         (u64)new_ppn,
                         aligned);

                /*
                 * Step 2: Zero the page (anonymous mapping semantics)
                 *
                 * For anonymous mappings (MAP_ANONYMOUS), the page must be
                 * zeroed before first use to prevent information leakage.
                 * We use KERNEL_PHY_TO_VIRT to get a kernel virtual address
                 * for the physical page, then memset to zero it.
                 */
                vaddr virt_page = KERNEL_PHY_TO_VIRT(PADDR(new_ppn));
                memset((void *)virt_page, 0, PAGE_SIZE);

                pr_debug(
                        "[Linux compat] Zeroed page at vaddr=0x%lx (ppn=0x%lx)\n",
                        virt_page,
                        (u64)new_ppn);

                /*
                 * Step 3: Map the page into user address space
                 *
                 * Use map() to establish the page table entry with the
                 * permissions from nexus.
                 */
                e = map(vs, new_ppn, VPN(aligned), 3, nflags, handler);
                if (e != REND_SUCCESS) {
                        pr_error("[Linux compat] Failed to map page (e=%d)\n",
                                 (int)e);
                        pmm->pmm_free(pmm, new_ppn, 1);
                        goto unhandled_fault;
                }

                /*
                 * Step 4: Update reverse mapping (rmap)
                 *
                 * The rmap links physical pages to virtual mappings, which
                 * is needed for page reclamation, swap, and COW tracking.
                 * For now, we skip this since we're not implementing those
                 * features yet.
                 *
                 * TODO: Link rmap list when we have the nexus_node
                 */

                pr_info("[Linux compat] Lazy allocation successful: va=0x%lx -> ppn=0x%lx\n",
                        aligned,
                        (u64)new_ppn);

                /* Page successfully allocated and mapped - return to user */
                return;
        }

        /*
         * Category 3: COW write fault.
         * Page present + read-only, with nexus saying writable.
         *
         * We determine this is a write fault based on nexus flags, not hardware
         * registers.
         */
        if (page_mapped && nexus_is_write && is_present
            && !(pt_flags & PAGE_ENTRY_WRITE)) {
                if (in_nexus && (nflags & PAGE_ENTRY_WRITE)) {
                        /* This is a COW page - nexus says writable, PTE is RO
                         */
                        pr_debug("[Linux compat] COW write fault at 0x%lx\n",
                                 fault_addr);
                        error_t e = linux_handle_cow_fault(
                                fault_addr, nexus_is_write, is_present);
                        if (e == REND_SUCCESS) {
                                return;
                        }
                        pr_error(
                                "[Linux compat] COW handling failed at 0x%lx: e=%d\n",
                                fault_addr,
                                (int)e);
                        goto unhandled_fault;
                } else {
                        /* Not COW - this is a true read-only violation */
                        goto handle_read_only_violation;
                }
        }

        /*
         * Category 4: True read-only violation.
         * Attempt to write to genuinely read-only mapping.
         */
        if (nexus_is_write && !is_present && page_mapped) {
        handle_read_only_violation:
                pr_error("[Linux compat] Write to read-only mapping at 0x%lx\n",
                         fault_addr);
                pr_error("[Linux compat] pt_flags=0x%lx, nflags=0x%lx\n",
                         (u64)pt_flags,
                         (u64)nflags);
                goto unhandled_fault;
        }

        /*
         * Category 5: Unmapped address (no nexus entry).
         * True segmentation fault - accessing unmapped memory.
         */
        if (!in_nexus) {
                pr_error("[Linux compat] Access to unmapped address at 0x%lx\n",
                         fault_addr);
                pr_error(
                        "[Linux compat] Not in nexus - this is a true segfault\n");
                goto unhandled_fault;
        }

        /*
         * Category 6: Other faults (execution violations, etc.)
         */
        pr_error("[Linux compat] Unhandled page fault at 0x%lx\n", fault_addr);
        pr_error(
                "[Linux compat] Fault details: write=%d, present=%d, exec=%d\n",
                is_write,
                is_present,
                is_execute);

fatal_fault:
unhandled_fault:
        if (!is_kernel) {
                /*
                 * User-mode fatal fault (SIGSEGV equivalent).
                 * Do not halt the kernel; terminate the current task.
                 */
                linux_fatal_user_fault(-LINUX_EFAULT);
                return;
        }

        /* Kernel-mode fault: dump and stop. */
        arch_unknown_trap_handler(tf);
        kernel_panic("Unhandled kernel page fault\n");
}

static void linux_page_fault_irq_init(void)
{
        /*
         * Register page fault handler using architecture-independent
         * trap_class.
         *
         * This automatically maps to the correct trap IDs on each architecture:
         * - x86_64: vector 14 (#PF)
         * - aarch64: EC 0x20 (instruction abort) and EC 0x24 (data abort)
         */
        register_fixed_trap(
                TRAP_CLASS_PAGE_FAULT, linux_trap_pf_handler, IRQ_NO_ATTR);
}

DEFINE_INIT_LEVEL(linux_page_fault_irq_init, 0);
