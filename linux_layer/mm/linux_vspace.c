#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/vspace_copy.h>
#include <rendezvos/mm/vmm.h>
#include <modules/log/log.h>

/*
 * Vspace copy for fork (COW prep, leaf-only).
 *
 * Core `clone_vspace(COW_PREP)` already clears PTE WRITE on shared pages and
 * tags radix COW. Compat fault/split must honor radix write intent; do not
 * reinstall every COW leaf here — that was a wrong chase for #PF 0x57f485
 * (stale SIGCHLD handler after execve, not missing RO PTEs).
 */

error_t linux_copy_vspace(VSpace *parent_vs, VSpace **child_vs_ptr)
{
        error_t e;

        if (!parent_vs || !child_vs_ptr) {
                return -E_IN_PARAM;
        }

        if (!linux_vspace_is_user_table(parent_vs)) {
                pr_error("[MM] Parent vspace is not a table vspace\n");
                return -E_IN_PARAM;
        }

        e = clone_vspace(parent_vs,
                         child_vs_ptr,
                         (enum vspace_clone_flags)(VSPACE_CLONE_F_USER_4K_ONLY
                                                   | VSPACE_CLONE_F_COW_PREP));
        if (e != REND_SUCCESS) {
                return e;
        }

        e = register_vspace(*child_vs_ptr, &root_vspace);
        if (e != REND_SUCCESS) {
                del_vspace(child_vs_ptr);
                return e;
        }

        return REND_SUCCESS;
}
