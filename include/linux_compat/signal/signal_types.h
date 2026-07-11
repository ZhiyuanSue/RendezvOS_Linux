#ifndef _LINUX_COMPAT_SIGNAL_TYPES_H_
#define _LINUX_COMPAT_SIGNAL_TYPES_H_

#include <common/types.h>
#include <rendezvos/task/id.h>

/* Forward declare clock_t if not available */
#ifndef clock_t
typedef long clock_t;
#endif

/* Linux-compatible type definitions */
#ifndef uid_t
typedef unsigned int uid_t;
#endif

/*
 * Linux Signal Types and Structures for Phase 2B
 *
 * This file defines signal-related data structures for implementing
 * the Linux signal mechanism in the linux_layer.
 */

/*
 * Standard signal numbers (1-31)
 * These match Linux signal definitions.
 */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGIOT    6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPOLL   SIGIO
#define SIGPWR    30
#define SIGSYS    31
#define SIGUNUSED 31

/* Real-time signal range */
#define SIGRTMIN 32
#define SIGRTMAX 64

/* Total number of signals */
#define _NSIG 64
#define NSIG  _NSIG

/*
 * Signal handler function types
 */
#define SIG_DFL ((__sighandler_t)0) /* Default handling */
#define SIG_IGN ((__sighandler_t)1) /* Ignore signal */
#define SIG_ERR ((__sighandler_t) - 1) /* Error return */

typedef void (*__sighandler_t)(int);

static inline bool linux_signal_handler_is_ign(__sighandler_t handler)
{
        return (uintptr_t)handler == (uintptr_t)SIG_IGN;
}

static inline bool linux_signal_handler_is_dfl(__sighandler_t handler)
{
        return (uintptr_t)handler == (uintptr_t)SIG_DFL;
}

static inline bool linux_signal_handler_is_special(__sighandler_t handler)
{
        return (uintptr_t)handler <= 1;
}

/*
 * sigset_t: Signal set (bitmap of signals)
 * Linux uses 64-bit signal sets for real-time signals.
 */
typedef struct {
        unsigned long sig[64 / (8 * sizeof(unsigned long))];
} sigset_t;

/*
 * siginfo_t: Signal information structure
 * This is passed to SA_SIGINFO signal handlers.
 */
typedef struct {
        int si_signo; /* Signal number */
        int si_errno; /* An errno value */
        int si_code; /* Signal code */
        int si_pad; /* Padding */

        union {
                struct {
                        pid_t si_pid; /* Sending process ID */
                        uid_t si_uid; /* Real user ID of sending process */
                } _kill;

                struct {
                        void *si_addr; /* Faulting address */
                        short si_addr_lsb; /* LSB of address */
                } _fault;

                struct {
                        int si_status; /* Exit value or signal */
                        clock_t si_utime; /* User time consumed */
                        clock_t si_stime; /* System time consumed */
                } _child;

                struct {
                        long si_band; /* Band event */
                        int si_fd; /* File descriptor */
                } _poll;

                struct {
                        void *si_call_addr; /* Address of system call */
                        int si_syscall; /* System call number */
                        unsigned int si_arch; /* Architecture */
                } _sys;

                struct {
                        union sigval {
                                int sival_int;
                                void *sival_ptr;
                        } si_value;
                } _rt;
        } _sifields;
} siginfo_t;

/* Helper macros for siginfo_t fields */
#define si_pid    _sifields._kill.si_pid
#define si_uid    _sifields._kill.si_uid
#define si_addr   _sifields._fault.si_addr
#define si_status _sifields._child.si_status
#define si_value  _sifields._rt.si_value

/*
 * struct sigaction — must match Linux uapi layout (arch-generic/signal.h).
 * x86_64/aarch64: sa_handler, sa_flags (unsigned long), sa_restorer, sa_mask.
 */
typedef struct {
        __sighandler_t sa_handler;
        unsigned long sa_flags;
        void (*sa_restorer)(void);
        sigset_t sa_mask;
} sigaction_t;

/*
 * Signal action flags (sa_flags)
 */
#define SA_NOCLDSTOP 0x00000001 /* Don't notify on child stop */
#define SA_NOCLDWAIT 0x00000002 /* Don't create zombies on child terminate */
#define SA_SIGINFO   0x00000004 /* Extended signal info (3-arg handler) */
#define SA_ONSTACK   0x08000000 /* Use alternate signal stack */
#define SA_RESTART   0x10000000 /* Restart syscall after handler */
#define SA_NODEFER   0x40000000 /* Don't block signal during handler */
#define SA_RESETHAND 0x80000000 /* Reset to SIG_DFL on entry */
#define SA_RESTORER  0x04000000 /* Use custom restorer function */

/*
 * Alternate signal stack
 */
typedef struct {
        void *ss_sp; /* Stack base address */
        int ss_flags; /* Flags (SS_DISABLE, SS_ONSTACK) */
        size_t ss_size; /* Stack size */
} stack_t;

/* Stack flags */
#define SS_ONSTACK    0x00000001 /* Currently executing on alternate stack */
#define SS_DISABLE    0x00000002 /* Alternate signal stack disabled */
#define SS_AUTODISARM 0x40000000 /* Auto-disable on entry (Linux 4.7+) */

/* Signal stack constants */
#define SIGSTKSZ    8192 /* Default alternate stack size */
#define MINSIGSTKSZ 2048 /* Minimum alternate stack size */

/*
 * Signal manipulation operations for rt_sigprocmask
 */
#define SIG_BLOCK   0 /* Block signals */
#define SIG_UNBLOCK 1 /* Unblock signals */
#define SIG_SETMASK 2 /* Set signal mask */

/*
 * Signal set manipulation functions
 */
static inline void sigemptyset(sigset_t *set)
{
        for (int i = 0; i < (int)(64 / (8 * sizeof(unsigned long))); i++) {
                set->sig[i] = 0;
        }
}

static inline void sigfillset(sigset_t *set)
{
        for (int i = 0; i < (int)(64 / (8 * sizeof(unsigned long))); i++) {
                set->sig[i] = ~0UL;
        }
}

static inline void sigaddset(sigset_t *set, int signo)
{
        if (signo >= 1 && signo <= 64) {
                set->sig[(signo - 1) / (8 * sizeof(unsigned long))] |=
                        (1UL << ((signo - 1) % (8 * sizeof(unsigned long))));
        }
}

static inline void sigdelset(sigset_t *set, int signo)
{
        if (signo >= 1 && signo <= 64) {
                set->sig[(signo - 1) / (8 * sizeof(unsigned long))] &=
                        ~(1UL << ((signo - 1) % (8 * sizeof(unsigned long))));
        }
}

static inline int sigismember(const sigset_t *set, int signo)
{
        if (signo < 1 || signo > 64) {
                return 0;
        }
        return !!(set->sig[(signo - 1) / (8 * sizeof(unsigned long))]
                  & (1UL << ((signo - 1) % (8 * sizeof(unsigned long)))));
}

#endif /* _LINUX_COMPAT_SIGNAL_TYPES_H_ */