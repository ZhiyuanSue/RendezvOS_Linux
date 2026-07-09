#include <modules/log/log.h>
#include <linux_compat/initcall.h>
#include <rendezvos/error.h>
#include <rendezvos/task/initcall.h>

#ifdef LINUX_COMPAT_TEST
/*
 * linux compat user tests are driven by `linux_layer/tests/user_test_runner.c`.
 * Placeholder only; global init belongs in BSP-once initcalls (see initcall.h).
 */
static void linux_compat_test_placeholder(void)
{
        if (!linux_init_on_bsp())
                return;
        pr_debug("[ Linux compat ] placeholder initcall\n");
}

DEFINE_INIT_LEVEL(linux_compat_test_placeholder, 6);
#endif
