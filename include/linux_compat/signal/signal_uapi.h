#ifndef _LINUX_COMPAT_SIGNAL_UAPI_H_
#define _LINUX_COMPAT_SIGNAL_UAPI_H_

#include <common/types.h>
#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/signal/signal_types.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/vmm.h>

/*
 * Userspace struct sigaction layout (Linux uapi, x86_64/aarch64 lp64):
 *   sa_handler, sa_flags, sa_restorer, sa_mask
 */
#define LINUX_UAPI_SIGACTION_SIZE         ((u64)sizeof(sigaction_t))
#define LINUX_UAPI_SIGACTION_OFF_FLAGS    8
#define LINUX_UAPI_SIGACTION_OFF_RESTORER 16
#define LINUX_UAPI_SIGACTION_OFF_MASK     24

static inline bool linux_sigsetsize_valid(u64 sigsetsize)
{
        return sigsetsize == (u64)sizeof(sigset_t);
}

static inline i64 linux_mm_errno_from_copy(error_t e)
{
        if (e == REND_SUCCESS) {
                return 0;
        }
        return -LINUX_EFAULT;
}

static inline error_t linux_copy_sigaction_from_user(VSpace* vs, u64 user_act,
                                                     sigaction_t* out)
{
        if (!vs || !out || user_act == 0) {
                return -E_IN_PARAM;
        }
        return linux_mm_load_from_user(
                vs, user_act, out, (size_t)LINUX_UAPI_SIGACTION_SIZE);
}

static inline error_t linux_copy_sigaction_to_user(VSpace* vs, u64 user_act,
                                                   const sigaction_t* in)
{
        if (!vs || !in || user_act == 0) {
                return -E_IN_PARAM;
        }
        return linux_mm_store_to_user(
                vs, user_act, in, (size_t)LINUX_UAPI_SIGACTION_SIZE);
}

#endif /* _LINUX_COMPAT_SIGNAL_UAPI_H_ */
