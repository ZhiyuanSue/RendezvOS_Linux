#include <modules/log/log.h>
#include <rendezvos/error.h>
#include <rendezvos/task/initcall.h>

#ifdef TEST
extern int task_test(void);
extern int elf_read_test(void);

static void linux_compat_test_init(void)
{
        pr_info("[ Linux compat ] running payload tests\n");
        if (elf_read_test() != REND_SUCCESS) {
                pr_error("[ Linux compat ] elf_read_test failed\n");
        }
        if (task_test() != REND_SUCCESS) {
                pr_error("[ Linux compat ] task_test failed\n");
        }
}

DEFINE_INIT_LEVEL(linux_compat_test_init, 6);
#endif
