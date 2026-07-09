#include <RendezvOS_Linux.h>
#include <linux_compat/initcall.h>
#include <rendezvos/task/initcall.h>

static bool linux_main_init_done;

void main_init()
{
        if (!linux_init_bsp_once(&linux_main_init_done))
                return;
        init_syscall_entry();
        linux_init_bsp_mark_done(&linux_main_init_done);
        return;
}
DEFINE_INIT_LEVEL(main_init, 0);
