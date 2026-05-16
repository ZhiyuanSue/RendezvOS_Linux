#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/vspace_copy.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/smp/percpu.h>
#include <modules/log/log.h>

/*
 * Vspace copy for fork implementation (COW prep, leaf-only).
 *
 * Contract (current stage):
 * - User space uses 4K pages only (no huge pages).
 * - For each mapped user page:
 *   - Child maps the same physical page at the same VA, but read-only if the
 *     parent's mapping was writable.
 *   - Parent's mapping is also downgraded to read-only for originally-writable
 *     pages (via map() remap of the same physical page with updated flags).
 * - Child radix metadata is populated by core clone_vspace (internal).
 *
 * This prepares COW semantics; the actual fault-time split is handled
 * elsewhere.
 */

error_t linux_copy_vspace(VSpace *parent_vs, VSpace **child_vs_ptr)
{
        if (!parent_vs || !child_vs_ptr) {
                return -E_IN_PARAM;
        }

        if (!linux_vspace_is_user_table(parent_vs)) {
                pr_error("[VSPACE_COPY] Parent vspace is not a table vspace\n");
                return -E_IN_PARAM;
        }

        return clone_vspace(
                parent_vs,
                child_vs_ptr,
                (enum vspace_clone_flags)(VSPACE_CLONE_F_USER_4K_ONLY
                                          | VSPACE_CLONE_F_COW_PREP));
}
