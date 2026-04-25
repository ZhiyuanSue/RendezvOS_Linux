#include <linux_compat/vspace_copy.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/mm/nexus.h>
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
 * - Nexus metadata is populated for the child by core vspace_clone (internal).
 *
 * This prepares COW semantics; the actual fault-time split is handled
 * elsewhere.
 */

error_t linux_copy_vspace(VS_Common *parent_vs, VS_Common **child_vs_ptr)
{
        if (!parent_vs || !child_vs_ptr) {
                return -E_IN_PARAM;
        }

        if (!vs_common_is_table_vspace(parent_vs)) {
                pr_error("[VSPACE_COPY] Parent vspace is not a table vspace\n");
                return -E_IN_PARAM;
        }

        return vspace_clone(parent_vs,
                            child_vs_ptr,
                            (vspace_clone_flags_t)(VSPACE_CLONE_F_USER_4K_ONLY
                                                   | VSPACE_CLONE_F_COW_PREP),
                            percpu(nexus_root));
}
