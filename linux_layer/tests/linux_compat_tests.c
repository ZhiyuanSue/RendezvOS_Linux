#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/task/initcall.h>

#ifdef LINUX_COMPAT_TEST
/*
 * linux compat user tests are driven by `linux_layer/tests/user_test_runner.c`.
 *
 * Keep this file as a placeholder for additional non-user tests, but do not
 * spawn all apps from every CPU: initcalls run on all CPUs.
 */
static void linux_compat_test_placeholder(void)
{
        pr_debug("[ Linux compat ] placeholder initcall\n");
}

DEFINE_INIT_LEVEL(linux_compat_test_placeholder, 6);
#endif
