#include <linux_compat/append_hooks.h>
#include <linux_compat/clone_flags.h>
#include <linux_compat/ipc/clean_protocol.h>
#include <linux_compat/proc/linux_exec.h>
#include <linux_compat/proc/linux_exec_proc.h>

#include <common/align.h>
#include <common/stddef.h>
#include <common/string.h>
#include <common/types.h>
#include <common/dsa/list.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/fs/linux_fd_table.h>
#include <linux_compat/fs/vfs_protocol.h>
#include <linux_compat/ipc/rpc.h>
#include <linux_compat/proc/wait_ipc.h>
#include <linux_compat/proc_registry.h>
#include <linux_compat/signal/signal_state.h>
#include <linux_compat/time/linux_time_sleep.h>
#include <linux_compat/initcall.h>
#include <modules/log/log.h>
#include <rendezvos/task/thread.h>
#include <rendezvos/task/initcall.h>
#include <rendezvos/ipc/ipc.h>
#include <rendezvos/ipc/port.h>
#include <rendezvos/mm/page_slice.h>

extern struct Port_Table *global_port_table;

const thread_append_hooks_t linux_thread_append_hooks = {
        .append_info_len = LINUX_THREAD_APPEND_BYTES,
        .init = NULL, /* user image: linux_exec_replace_image / prepare_new */
        .copy = linux_thread_append_copy,
        .fini = linux_thread_append_fini,
};

void linux_thread_append_fini(Thread_Base *thread)
{
        Thread_Base *thr = thread;
        char kport[PORT_NAME_LEN_MAX];
        const char *pfx = "vfs_cli_k_t";
        size_t plen;
        size_t i;

        if (!thr) {
                return;
        }

        /*
         * Backend RPC reply ports are vfs_cli_k_t<tid>. Drop on thread
         * teardown so the global name table does not grow without bound
         * across run_all forks (and a reused tid cannot hit a stale port).
         */
        plen = strlen(pfx);
        if (plen < sizeof(kport)) {
                memcpy(kport, pfx, plen);
                i = plen;
                if (proc_format_pid(
                            kport + i, sizeof(kport) - i, (pid_t)thr->tid)
                    != 0) {
                        ipc_rpc_unregister_port_name(kport);
                }
        }

        linux_time_sleep_port_teardown(thr);
        linux_signal_thread_destroy(thr);
        linux_proc_detach_thread(thr);
}

error_t linux_thread_append_copy(Thread_Base *dst, Thread_Base *src)
{
        Thread_Base *d = dst;
        Thread_Base *s = src;
        linux_thread_append_t *dst_ta;

        if (!d) {
                return -E_IN_PARAM;
        }

        dst_ta = linux_thread_append(d);
        if (!dst_ta) {
                return REND_SUCCESS;
        }

        /* Fork/clone policy: child must not inherit runner cookie or clear_tid.
         */
        dst_ta->boot_wait_cookie = 0;
        dst_ta->clear_tid = 0;
        dst_ta->res = NULL;
        INIT_LIST_HEAD(&dst_ta->res_thread_node);

        return linux_signal_thread_fork_inherit(d, s, true);
}

static bool linux_elf_init_logged;

static void linux_elf_init_initcall(void)
{
        if (!linux_init_bsp_once(&linux_elf_init_logged))
                return;
        pr_info("[LINUX_ELF_INIT] Module initialized\n");
        linux_init_bsp_mark_done(&linux_elf_init_logged);
}
DEFINE_INIT(linux_elf_init_initcall);
