#include <common/stdbool.h>
#include <common/types.h>
#include <common/string.h>
#include <linux_compat/errno.h>
#include <linux_compat/linux_mm_radix.h>
#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_types.h>
#include <linux_compat/signal/signal_context.h>
#include <linux_compat/signal/signal_altstack.h>
#include <linux_compat/signal/signal_state.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/vmm.h>
#include <rendezvos/task/tcb.h>
#include <rendezvos/trap/trap.h>
#include <syscall.h>
#include <syscall_entry.h>

/* Below typical stack at USER_SPACE_TOP; search upward for a free page. */
#define SIGNAL_SIGRETURN_MAP_HINT ((vaddr)0x0000700000000000ULL)

static bool signal_thread_has_pending(linux_signal_thread_state_t *ts)
{
        if (!ts) {
                return false;
        }

        for (int i = 0; i < (int)(64 / (8 * sizeof(unsigned long))); i++) {
                if (ts->pending_signals.sig[i] != 0) {
                        return true;
                }
        }

        return false;
}

static int signal_select_pending(linux_signal_thread_state_t *ts,
                                 linux_signal_proc_state_t *ps)
{
        (void)ps;

        if (!ts) {
                return 0;
        }

        if (sigismember(&ts->pending_signals, SIGKILL)) {
                return SIGKILL;
        }
        if (sigismember(&ts->pending_signals, SIGSTOP)) {
                return SIGSTOP;
        }

        for (int sig = 1; sig <= 31; sig++) {
                if (sigismember(&ts->pending_signals, sig)
                    && !sigismember(&ts->blocked_signals, sig)) {
                        return sig;
                }
        }

        for (int sig = 32; sig <= 64; sig++) {
                if (sigismember(&ts->pending_signals, sig)
                    && !sigismember(&ts->blocked_signals, sig)) {
                        return sig;
                }
        }

        return 0;
}

static void signal_apply_handler_mask(linux_signal_thread_state_t *ts,
                                      const sigaction_t *disp, int sig)
{
        if (!ts || !disp) {
                return;
        }

        if (!(disp->sa_flags & SA_NODEFER)) {
                sigaddset(&ts->blocked_signals, sig);
        }

        for (int s = 1; s <= NSIG; s++) {
                if (sigismember(&disp->sa_mask, s)) {
                        sigaddset(&ts->blocked_signals, s);
                }
        }
}

static void signal_clear_pending(linux_signal_thread_state_t *ts,
                                 linux_signal_proc_state_t *ps, int sig)
{
        if (ts) {
                sigdelset(&ts->pending_signals, sig);
        }
        if (ps) {
                sigdelset(&ps->pending_signals, sig);
        }
}

static bool signal_save_handler_context(Thread_Base *th,
                                        linux_signal_thread_state_t *ts,
                                        struct trap_frame *tf)
{
        linux_signal_restore_t *rs;

        if (!ts) {
                return false;
        }

        rs = &ts->signal_restore;

        if (ts->signal_inflight != 0 || rs->active) {
                return false;
        }

        rs->active = 1;
        ts->signal_inflight = 1;
        rs->saved_blocked = ts->blocked_signals;
        linux_signal_arch_save_context(tf, th ? &th->ctx : NULL, rs);
        return true;
}

/*
 * Ensure a per-process RX page with `rt_sigreturn` stub (mini-VDSO).
 * Map RW → write instructions → drop WRITE / set EXEC (avoid WX on stack).
 */
static vaddr signal_ensure_sigreturn_page(VSpace *vs,
                                          linux_signal_proc_state_t *ps)
{
        ENTRY_FLAGS_t rw_flags;
        ENTRY_FLAGS_t rx_flags;
        void *mapped;
        vaddr page;

        if (!vs || !ps)
                return 0;
        if (ps->sigreturn_page)
                return ps->sigreturn_page;

        rw_flags = PAGE_ENTRY_USER | PAGE_ENTRY_VALID | PAGE_ENTRY_READ
                   | PAGE_ENTRY_WRITE;
        mapped = linux_mm_map_user_range_search(
                vs, SIGNAL_SIGRETURN_MAP_HINT, 1, rw_flags, 512);
        if (!mapped)
                return 0;

        page = (vaddr)(uintptr_t)mapped;

#if defined(_AARCH64_)
        {
                u32 stub[2];

                stub[0] = 0xD2800000u
                          | (((u32)__NR_rt_sigreturn & 0xFFFFu) << 5) | 8u;
                stub[1] = 0xD4000001u;
                if (linux_mm_store_to_user(vs, (u64)page, stub, sizeof(stub))
                    != REND_SUCCESS) {
                        (void)linux_mm_unmap_user_range(vs, page, 1);
                        return 0;
                }
        }
#elif defined(_X86_64_)
        {
                u32 nr = (u32)__NR_rt_sigreturn;
                u8 stub[9];

                stub[0] = 0x48;
                stub[1] = 0xc7;
                stub[2] = 0xc0;
                stub[3] = (u8)(nr & 0xff);
                stub[4] = (u8)((nr >> 8) & 0xff);
                stub[5] = (u8)((nr >> 16) & 0xff);
                stub[6] = (u8)((nr >> 24) & 0xff);
                stub[7] = 0x0f;
                stub[8] = 0x05;
                if (linux_mm_store_to_user(vs, (u64)page, stub, sizeof(stub))
                    != REND_SUCCESS) {
                        (void)linux_mm_unmap_user_range(vs, page, 1);
                        return 0;
                }
        }
#else
        (void)linux_mm_unmap_user_range(vs, page, 1);
        return 0;
#endif

        rx_flags = PAGE_ENTRY_USER | PAGE_ENTRY_VALID | PAGE_ENTRY_READ
                   | PAGE_ENTRY_EXEC;
        if (linux_mm_update_range_flags(vs, page, PAGE_SIZE, rx_flags)
            != REND_SUCCESS) {
                (void)linux_mm_unmap_user_range(vs, page, 1);
                return 0;
        }

        ps->sigreturn_page = page;
        return page;
}

/*
 * Arrange a return path from the user handler into rt_sigreturn.
 *
 * Prefer SA_RESTORER; else per-process RX stub page (never RW+X stack —
 * see WAIT_AND_SIGCHLD.md §5).
 */
static bool signal_install_return_path(VSpace *vs, struct trap_frame *tf,
                                       vaddr *user_sp_inout,
                                       const sigaction_t *disp,
                                       linux_signal_proc_state_t *ps)
{
        vaddr restorer;
        vaddr sp;

        if (!vs || !tf || !user_sp_inout || !disp || !ps)
                return false;

        if ((disp->sa_flags & SA_RESTORER) && disp->sa_restorer
            && (uintptr_t)disp->sa_restorer >= (uintptr_t)PAGE_SIZE) {
                restorer = (vaddr)(uintptr_t)disp->sa_restorer;
        } else {
                restorer = signal_ensure_sigreturn_page(vs, ps);
                if (!restorer)
                        return false;
        }

        sp = *user_sp_inout;

#if defined(_AARCH64_)
        tf->REGS[30] = (u64)restorer;
        *user_sp_inout = sp & ~((vaddr)0xF);
        return true;
#elif defined(_X86_64_)
        {
                u64 ret_addr = (u64)restorer;

                sp = (sp - sizeof(u64)) & ~((vaddr)0xF);
                if (linux_mm_store_to_user(vs, (u64)sp, &ret_addr,
                                           sizeof(ret_addr))
                    != REND_SUCCESS)
                        return false;
        }
        *user_sp_inout = sp;
        return true;
#else
        (void)restorer;
        (void)sp;
        return false;
#endif
}

bool linux_signal_thread_has_deliverable_pending(Thread_Base *thread)
{
        Tcb_Base *process;
        linux_signal_thread_state_t *ts;
        linux_signal_proc_state_t *ps;

        if (!thread) {
                return false;
        }

        process = thread->belong_tcb;
        if (!process) {
                return false;
        }

        ts = linux_signal_thread_state(thread);
        ps = linux_signal_proc_state(process);
        if (!ts || !ps) {
                return false;
        }

        return signal_select_pending(ts, ps) != 0;
}

bool linux_signal_wait4_should_return_eintr(Thread_Base *thread)
{
        Tcb_Base *process;
        linux_signal_thread_state_t *ts;
        linux_signal_proc_state_t *ps;
        int sig;

        if (!thread) {
                return false;
        }

        process = thread->belong_tcb;
        if (!process) {
                return false;
        }

        ts = linux_signal_thread_state(thread);
        ps = linux_signal_proc_state(process);
        if (!ts || !ps) {
                return false;
        }

        sig = signal_select_pending(ts, ps);
        if (sig == 0) {
                return false;
        }

        /*
         * protocols/WAIT_AND_SIGCHLD.md — Channel S must not EINTR wait4.
         * EXIT_NOTIFY (Channel R) is the only authoritative wait wake for
         * child exit. SIGCHLD stays pending for Layer B; other signals EINTR.
         */
        if (sig == SIGCHLD) {
                return false;
        }

        return true;
}

bool linux_signal_has_deliverable_pending(void)
{
        Thread_Base *current_thread = get_cpu_current_thread();
        linux_signal_thread_state_t *ts;

        if (!current_thread) {
                return false;
        }

        ts = linux_signal_thread_state(current_thread);
        if (!ts || !signal_thread_has_pending(ts)) {
                return false;
        }

        return linux_signal_thread_has_deliverable_pending(current_thread);
}

bool linux_deliver_pending_signals(struct trap_frame *tf)
{
        Thread_Base *current_thread = get_cpu_current_thread();
        Tcb_Base *current_process;
        linux_signal_thread_state_t *ts;
        linux_signal_proc_state_t *ps;
        int sig;
        sigaction_t *disp;

        if (!current_thread) {
                return false;
        }

        ts = linux_signal_thread_state(current_thread);
        if (!ts || !signal_thread_has_pending(ts)) {
                return false;
        }

        current_process = current_thread->belong_tcb;
        if (!current_process) {
                return false;
        }

        ps = linux_signal_proc_state(current_process);
        if (!ps) {
                return false;
        }

        sig = signal_select_pending(ts, ps);
        if (sig == 0) {
                return false;
        }

        disp = &ps->dispositions[sig - 1];

        if (linux_signal_handler_is_ign(disp->sa_handler)) {
                signal_clear_pending(ts, ps, sig);
                return false;
        }

        if (linux_signal_handler_is_dfl(disp->sa_handler)) {
                switch (sig) {
                case SIGHUP:
                case SIGINT:
                case SIGTERM:
                case SIGUSR1:
                case SIGUSR2:
                case SIGPIPE:
                case SIGALRM:
                case SIGPROF:
                case SIGVTALRM:
                case SIGSTKFLT:
                case SIGPWR:
                        signal_clear_pending(ts, ps, sig);
                        sys_exit(128 + sig);
                        __builtin_unreachable();

                case SIGCHLD:
                case SIGCONT:
                case SIGWINCH:
                case SIGURG:
                        signal_clear_pending(ts, ps, sig);
                        return false;

                case SIGQUIT:
                case SIGILL:
                case SIGTRAP:
                case SIGABRT:
                case SIGBUS:
                case SIGFPE:
                case SIGSEGV:
                case SIGXCPU:
                case SIGXFSZ:
                        signal_clear_pending(ts, ps, sig);
                        sys_exit(128 + sig);
                        __builtin_unreachable();

                case SIGSTOP:
                case SIGTSTP:
                case SIGTTIN:
                case SIGTTOU:
                        signal_clear_pending(ts, ps, sig);
                        return false;

                default:
                        signal_clear_pending(ts, ps, sig);
                        return false;
                }
        }

        if ((uintptr_t)disp->sa_handler < PAGE_SIZE) {
                signal_clear_pending(ts, ps, sig);
                return false;
        }

        if (ts->signal_inflight != 0) {
                return false;
        }

        vaddr user_sp, syscall_ret, user_pc_discarded;

        arch_syscall_get_user_return(tf,
                                     current_thread ? &current_thread->ctx :
                                                      NULL,
                                     &user_pc_discarded,
                                     &user_sp,
                                     &syscall_ret);

        if ((disp->sa_flags & SA_ONSTACK) && ts->alt_stack.ss_sp != NULL
            && !(ts->alt_stack.ss_flags & SS_DISABLE)
            && ts->alt_stack.ss_size >= MINSIGSTKSZ) {
                stack_t *alt_stack = &ts->alt_stack;
                vaddr alt_base = (vaddr)(uintptr_t)alt_stack->ss_sp;

                if (!linux_signal_altstack_region_mapped(current_process->vs,
                                                         alt_base,
                                                         alt_stack->ss_size)) {
                        signal_clear_pending(ts, ps, sig);
                        sys_exit(128 + sig);
                        __builtin_unreachable();
                }

                if (!(alt_stack->ss_flags & SS_ONSTACK)) {
                        ts->saved_main_sp = user_sp;
                        user_sp = ((vaddr)(uintptr_t)alt_stack->ss_sp
                                   + alt_stack->ss_size)
                                  & ~((vaddr)0xF);
                        alt_stack->ss_flags |= SS_ONSTACK;
                }
        }

        if (!signal_save_handler_context(current_thread, ts, tf)) {
                return false;
        }
        ts->signal_restore.sig = sig;

        if (!current_process->vs
            || !signal_install_return_path(current_process->vs, tf, &user_sp,
                                           disp, ps)) {
                                                   ts->signal_inflight = 0;
                ts->signal_restore.active = 0;
                if (ts->alt_stack.ss_flags & SS_ONSTACK) {
                        ts->alt_stack.ss_flags &= ~SS_ONSTACK;
                }
                signal_clear_pending(ts, ps, sig);
                return false;
        }

        if (disp->sa_flags & SA_RESETHAND) {
                disp->sa_handler = SIG_DFL;
        }
        signal_apply_handler_mask(ts, disp, sig);
        arch_syscall_set_user_return(tf,
                                     current_thread ? &current_thread->ctx :
                                                      NULL,
                                     (vaddr)(uintptr_t)disp->sa_handler,
                                     user_sp,
                                     syscall_ret);
        arch_syscall_set_user_int_arg(tf, 0, (u64)sig);

        signal_clear_pending(ts, ps, sig);

        return true;
}
