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
#include <linux_compat/linux_mm_radix.h>
#include <rendezvos/mm/mm_user_utils.h>
#include <rendezvos/mm/vmm_radix_tree.h>
#include <rendezvos/mm/pmm.h>
#include <rendezvos/smp/percpu.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/limits.h>
#include <linux_compat/fault.h>
#include <linux_compat/errno.h>
#include <linux_compat/signal/signal_deliver.h>
#include <linux_compat/signal/signal_queue.h>
#include <linux_compat/signal/signal_types.h>

#if defined(_X86_64_)
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/boot/arch_setup.h>
#include <arch/x86_64/tcb_arch.h>
#elif defined(_AARCH64_)
#include <arch/aarch64/mm/pmm.h>
#include <arch/aarch64/boot/arch_setup.h>
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
 * 5. Use linux_mm_remap_user_leaf() / mm_user_utils_remap_page (radix + PTE +
 *    rmap + old page free).
 */
static error_t linux_handle_cow_fault(vaddr fault_addr, bool is_write,
                                      bool is_present)
{
        VSpace *vs = percpu(current_vspace);
        struct map_handler *handler = &percpu(Map_Handler);
        struct pmm *pmm = vs->pmm;

        if (!vs || !handler || !pmm) {
                pr_error("[MM] COW: NULL vs/handler/pmm\n");
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
         * - Radix_node_t: stores original permissions (e.g., RW)
         * - Page table: read-only (COW protection)
         *
         * When a write fault occurs on a COW page:
         * - old_flags (from page table): read-only
         * - But the page should be writable according to radix tree flags
         *
         * We can't directly check radix tree flags here without a query interface,
         * so we rely on the fact that:
         * - If page is mapped and read-only, it might be COW
         * - linux_mm_remap_user_leaf verifies radix tree state
         */
        if (old_flags & PAGE_ENTRY_WRITE) {
                /* Page already writable - not a COW fault */
                pr_error("[MM] COW: Page already writable at vaddr=0x%lx\n",
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
         * 1. A COW page (radix tree says writable, page table says read-only)
         * 2. A true read-only page (both radix tree and page table say read-only)
         *
         * We attempt COW split; remap helper distinguishes
         * between these cases and return appropriate error codes.
         */

        /* Step 3: Allocate new physical page */
        size_t alloced_page_number;
        ppn_t new_ppn = pmm->pmm_alloc(pmm, 1, &alloced_page_number);
        if (invalid_ppn(new_ppn) || alloced_page_number != 1) {
                pr_error("[MM] COW: Failed to allocate new physical page\n");
                return -E_RENDEZVOS;
        }

        /* Step 4: Copy page content using map_handler_copy_page() */
        error_t e = map_handler_copy_page(handler, new_ppn, old_ppn);
        if (e != REND_SUCCESS) {
                pr_error("[MM] COW: Failed to copy page content (e=%d)\n", (int)e);
                pmm->pmm_free(pmm, new_ppn, 1);
                return e;
        }

        if (!vs->root_radix) {
                pr_error("[MM] COW: vs has no radix root va=0x%lx\n", fault_addr);
                pmm->pmm_free(pmm, new_ppn, 1);
                return -E_RENDEZVOS;
        }

        ENTRY_FLAGS_t new_flags = old_flags | PAGE_ENTRY_WRITE;

        e = linux_mm_remap_user_leaf(
                vs, fault_addr, new_ppn, new_flags, old_ppn);

        if (e != REND_SUCCESS) {
                pr_error("[MM] COW: Failed to remap page (e=%d)\n", (int)e);
                pr_error(
                        "[MM] COW: Details: va=0x%lx, new_ppn=0x%lx, old_ppn=0x%lx, flags=0x%lx, entry_level=%d\n",
                        fault_addr,
                        (u64)new_ppn,
                        (u64)old_ppn,
                        (u64)new_flags,
                        entry_level);
                pmm->pmm_free(pmm, new_ppn, 1);
                return e;
        }

        return REND_SUCCESS;
}

static void linux_compat_deliver_segv_or_fatal(struct trap_frame* tf)
{
        Thread_Base* th = get_cpu_current_thread();
        Tcb_Base* task = th ? th->belong_tcb : NULL;

        if (task) {
                (void)linux_queue_signal(task, SIGSEGV, task->pid);
                if (linux_deliver_pending_signals(tf)) {
                        return;
                }
        }
        linux_fatal_user_fault(128 + SIGSEGV);
}

static void linux_trap_pf_handler(struct trap_frame *tf)
{
        vaddr fault_addr = arch_get_fault_addr(tf);
        vaddr aligned = ROUND_DOWN(fault_addr, PAGE_SIZE);
        bool page_mapped = false;
        bool in_radix = false;

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

        /*
         * First, determine if this is a kernel or user fault.
         * This must be done early for proper error handling.
         */
        bool is_kernel = arch_int_from_kernel(tf);

        if (fault_addr < PAGE_SIZE) {
                pr_error(
                        "[MM] NULL pointer access at 0x%lx (is_kernel=%d)\n",
                        fault_addr,
                        is_kernel);
                if (is_kernel) {
                        kernel_panic("NULL pointer dereference in kernel\n");
                } else {
                        linux_compat_deliver_segv_or_fatal(tf);
                }
                return;
        }

        VSpace *vs = percpu(current_vspace);
        struct map_handler *handler = &percpu(Map_Handler);

        if (!vs || !handler) {
                pr_error("[MM] NULL vs or handler\n");
                goto fatal_fault;
        }

        ENTRY_FLAGS_t pt_flags = 0;
        int level = 0;
        ppn_t ppn = have_mapped(vs, VPN(aligned), &pt_flags, &level, handler);

        vaddr nstart = 0;
        ENTRY_FLAGS_t nflags = 0;
        error_t ne = linux_mm_query_vaddr(vs, aligned, &nstart, &nflags);

        /*
         * Page fault handling decision tree based on radix shadow flags.
         * - Otherwise -> this is a read access
         *
         * User-mode faults: SIGSEGV (linux_fatal_user_fault)
         * Kernel-mode faults: kernel_panic
         */

        bool radix_is_write = (nflags & PAGE_ENTRY_WRITE);
        bool radix_is_exec = (nflags & PAGE_ENTRY_EXEC);

        page_mapped = !invalid_ppn(ppn) && (pt_flags & PAGE_ENTRY_VALID);
        in_radix = (ne == REND_SUCCESS);

        /*
         * Category 2: Lazy allocation.
         * Page not mapped, but exists in radix tree with appropriate permissions.
         *
         * This is the core of demand paging: allocate physical pages on first
         * access rather than pre-allocating. This saves memory and allows
         * sparse address spaces.
         */
        if (!page_mapped && in_radix) {
                if (nflags & PAGE_ENTRY_VALID) {
                        error_t re =
                                linux_mm_reinstall_user_pte(vs, aligned);
                        if (re == REND_SUCCESS)
                                return;
                        goto unhandled_fault;
                }

                if (radix_is_write && !(nflags & PAGE_ENTRY_WRITE)) {
                        pr_error(
                                "[MM] radix/ fault mismatch: write on RO lazy page at 0x%lx\n",
                                fault_addr);
                        goto handle_read_only_violation;
                }
                if (radix_is_exec && !(nflags & PAGE_ENTRY_EXEC)) {
                        pr_error(
                                "[MM] radix/ fault mismatch: exec on NX lazy page at 0x%lx\n",
                                fault_addr);
                        goto handle_read_only_violation;
                }

                vaddr page_end;
                if (!vmm_radix_tree_calculate_end_check(aligned, 1, &page_end))
                        goto unhandled_fault;

                vaddr l0_lo = ROUND_DOWN(aligned, (vaddr)HUGE_PAGE_SIZE);
                if (vmm_radix_tree_lock_range_big(vs, l0_lo, page_end)
                    != REND_SUCCESS)
                        goto unhandled_fault;

                error_t e = mm_user_utils_fill_page_with_exist_range(
                        vs, aligned, nflags);
                (void)vmm_radix_tree_unlock_range_big(vs, l0_lo, page_end);
                if (e != REND_SUCCESS)
                        goto unhandled_fault;

                return;
        }

        /*
         * Category 3: COW write fault.
         * Page present + read-only, with radix tree saying writable.
         *
         * We determine this is a write fault based on radix tree flags, not hardware
         * registers.
         */
        if (page_mapped && radix_is_write && is_present
            && !(pt_flags & PAGE_ENTRY_WRITE)) {
                if (in_radix && (nflags & PAGE_ENTRY_WRITE)) {
                        /* COW: radix writable, PTE read-only */
                        error_t e = linux_handle_cow_fault(
                                fault_addr, radix_is_write, is_present);
                        if (e == REND_SUCCESS) {
                                return;
                        }
                        pr_error(
                                "[MM] COW handling failed at 0x%lx: e=%d\n",
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
        if (radix_is_write && !is_present && page_mapped) {
        handle_read_only_violation:
                pr_error("[MM] Write to read-only mapping at 0x%lx\n",
                         fault_addr);
                pr_error("[MM] pt_flags=0x%lx, nflags=0x%lx\n",
                         (u64)pt_flags,
                         (u64)nflags);
                goto unhandled_fault;
        }

        /*
         * Category 5: Unmapped address (no radix tree entry).
         * True segmentation fault - accessing unmapped memory.
         */
        if (!in_radix) {
                pr_error("[MM] Access to unmapped address at 0x%lx\n",
                         fault_addr);
                pr_error(
                        "[MM] Not in radix - true segfault\n");
                goto unhandled_fault;
        }

        /*
         * Category 6: Other faults (execution violations, etc.)
         */
        pr_error("[MM] Unhandled page fault at 0x%lx\n", fault_addr);
        pr_error(
                "[MM] Fault details: write=%d, present=%d, exec=%d\n",
                is_write,
                is_present,
                is_execute);

fatal_fault:
unhandled_fault:
        if (!is_kernel) {
                linux_compat_deliver_segv_or_fatal(tf);
                return;
        }

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
