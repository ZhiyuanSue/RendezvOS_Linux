#include <RendezvOS_Linux.h>
#include <rendezvos/task/initcall.h>

void main_init()
{
        init_syscall_entry();
        return;
}
DEFINE_INIT_LEVEL(main_init, 0);
