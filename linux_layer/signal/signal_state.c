/*
 * Per-process / per-thread signal state on heap (linux_layer).
 */

#include <linux_compat/proc_compat.h>
#include <linux_compat/signal/signal_state.h>

#include <common/string.h>
#include <rendezvos/error.h>
#include <rendezvos/mm/allocator.h>
#include <rendezvos/smp/percpu.h>

static void linux_signal_init_proc_state(linux_signal_proc_state_t *ps)
{
        int i;

        if (!ps) {
                return;
        }

        sigemptyset(&ps->pending_signals);
        for (i = 0; i < NSIG; i++) {
                ps->dispositions[i].sa_handler = SIG_DFL;
                ps->dispositions[i].sa_flags = 0;
                sigemptyset(&ps->dispositions[i].sa_mask);
                ps->dispositions[i].sa_restorer = NULL;
        }
}

static void linux_signal_init_thread_state(linux_signal_thread_state_t *ts)
{
        if (!ts) {
                return;
        }

        sigemptyset(&ts->blocked_signals);
        sigemptyset(&ts->pending_signals);
        memset(&ts->alt_stack, 0, sizeof(ts->alt_stack));
        ts->saved_main_sp = 0;
        ts->signal_inflight = 0;
        memset(&ts->signal_restore, 0, sizeof(ts->signal_restore));
}

static linux_signal_proc_state_t *linux_signal_proc_alloc(void)
{
        struct allocator *alloc = percpu(kallocator);
        linux_signal_proc_state_t *ps;

        if (!alloc) {
                return NULL;
        }

        ps = (linux_signal_proc_state_t *)alloc->m_alloc(alloc, sizeof(*ps));
        if (ps) {
                linux_signal_init_proc_state(ps);
        }
        return ps;
}

static void linux_signal_proc_free(linux_signal_proc_state_t *ps)
{
        struct allocator *alloc = percpu(kallocator);

        if (!ps || !alloc) {
                return;
        }

        alloc->m_free(alloc, ps);
}

static linux_signal_thread_state_t *linux_signal_thread_alloc(void)
{
        struct allocator *alloc = percpu(kallocator);
        linux_signal_thread_state_t *ts;

        if (!alloc) {
                return NULL;
        }

        ts = (linux_signal_thread_state_t *)alloc->m_alloc(alloc, sizeof(*ts));
        if (ts) {
                linux_signal_init_thread_state(ts);
        }
        return ts;
}

static void linux_signal_thread_free(linux_signal_thread_state_t *ts)
{
        struct allocator *alloc = percpu(kallocator);

        if (!ts || !alloc) {
                return;
        }

        alloc->m_free(alloc, ts);
}

error_t linux_signal_proc_attach(Tcb_Base *task)
{
        linux_proc_append_t *pa;
        linux_signal_proc_state_t *ps;

        if (!task) {
                return -E_IN_PARAM;
        }

        pa = linux_proc_append(task);
        if (!pa) {
                return -E_IN_PARAM;
        }

        if (pa->signal) {
                linux_signal_init_proc_state(pa->signal);
                return REND_SUCCESS;
        }

        ps = linux_signal_proc_alloc();
        if (!ps) {
                return -E_RENDEZVOS;
        }

        pa->signal = ps;
        return REND_SUCCESS;
}

void linux_signal_proc_destroy(Tcb_Base *task)
{
        linux_proc_append_t *pa;

        if (!task) {
                return;
        }

        pa = linux_proc_append(task);
        if (!pa || !pa->signal) {
                return;
        }

        linux_signal_proc_free(pa->signal);
        pa->signal = NULL;
}

error_t linux_signal_proc_fork(Tcb_Base *child, Tcb_Base *parent)
{
        linux_proc_append_t *cpa;
        linux_proc_append_t *ppa;
        linux_signal_proc_state_t *child_ps;
        linux_signal_proc_state_t *parent_ps;
        error_t e;

        if (!child || !parent) {
                return -E_IN_PARAM;
        }

        cpa = linux_proc_append(child);
        ppa = linux_proc_append(parent);
        if (!cpa) {
                return -E_IN_PARAM;
        }

        e = linux_signal_proc_attach(child);
        if (e != REND_SUCCESS) {
                return e;
        }

        child_ps = cpa->signal;
        parent_ps = ppa ? ppa->signal : NULL;
        if (child_ps && parent_ps) {
                memcpy(child_ps->dispositions,
                       parent_ps->dispositions,
                       sizeof(child_ps->dispositions));
        }
        if (child_ps) {
                sigemptyset(&child_ps->pending_signals);
        }

        return REND_SUCCESS;
}

void linux_signal_proc_reset(Tcb_Base *task)
{
        linux_signal_proc_state_t *ps = linux_signal_proc_state(task);

        if (!ps) {
                return;
        }

        sigemptyset(&ps->pending_signals);
}

error_t linux_signal_thread_attach(Thread_Base *thread)
{
        linux_thread_append_t *ta;
        linux_signal_thread_state_t *ts;

        if (!thread) {
                return -E_IN_PARAM;
        }

        ta = linux_thread_append(thread);
        if (!ta) {
                return -E_IN_PARAM;
        }

        if (ta->signal) {
                linux_signal_init_thread_state(ta->signal);
                return REND_SUCCESS;
        }

        ts = linux_signal_thread_alloc();
        if (!ts) {
                return -E_RENDEZVOS;
        }

        ta->signal = ts;
        return REND_SUCCESS;
}

void linux_signal_thread_destroy(Thread_Base *thread)
{
        linux_thread_append_t *ta;

        if (!thread) {
                return;
        }

        ta = linux_thread_append(thread);
        if (!ta || !ta->signal) {
                return;
        }

        linux_signal_thread_free(ta->signal);
        ta->signal = NULL;
}

error_t linux_signal_thread_fork_inherit(Thread_Base *child,
                                         Thread_Base *parent,
                                         bool copy_blocked)
{
        linux_thread_append_t *cta;
        linux_thread_append_t *pta;
        sigset_t blocked;
        error_t e;

        if (!child) {
                return -E_IN_PARAM;
        }

        cta = linux_thread_append(child);
        if (!cta) {
                return -E_IN_PARAM;
        }

        sigemptyset(&blocked);
        pta = parent ? linux_thread_append(parent) : NULL;
        if (copy_blocked && pta && pta->signal) {
                blocked = pta->signal->blocked_signals;
        }

        e = linux_signal_thread_attach(child);
        if (e != REND_SUCCESS) {
                return e;
        }

        if (copy_blocked && cta->signal) {
                cta->signal->blocked_signals = blocked;
        }

        return REND_SUCCESS;
}

linux_signal_proc_state_t *linux_signal_proc_state(Tcb_Base *task)
{
        linux_proc_append_t *pa;

        if (!task) {
                return NULL;
        }

        pa = linux_proc_append(task);
        if (!pa) {
                return NULL;
        }

        if (!pa->signal) {
                if (linux_signal_proc_attach(task) != REND_SUCCESS) {
                        return NULL;
                }
        }

        return pa->signal;
}

void linux_signal_reinit_proc_state(linux_signal_proc_state_t *ps)
{
        linux_signal_init_proc_state(ps);
}

void linux_signal_reinit_thread_state(linux_signal_thread_state_t *ts)
{
        linux_signal_init_thread_state(ts);
}

linux_signal_thread_state_t *linux_signal_thread_state(Thread_Base *thread)
{
        linux_thread_append_t *ta;

        if (!thread) {
                return NULL;
        }

        ta = linux_thread_append(thread);
        if (!ta) {
                return NULL;
        }

        if (!ta->signal) {
                if (linux_signal_thread_attach(thread) != REND_SUCCESS) {
                        return NULL;
                }
        }

        return ta->signal;
}
