# Phase 2B: Signal Mechanism Analysis

**Status**: 🔍 **ANALYSIS IN PROGRESS**

**实施指南（trap 返回 / 双架构）**：[`SIGNAL_DELIVERY_TRAP_PATHS.md`](SIGNAL_DELIVERY_TRAP_PATHS.md)  
**IPC 边界**：[`IPC_BASED_SIGNAL_DESIGN.md`](IPC_BASED_SIGNAL_DESIGN.md)

## Overview

Phase 2B focuses on implementing the Linux signal mechanism, which is crucial for process communication, exception handling, and system operation. Signals are software interrupts that can be sent to processes or threads to notify them of events.

## Signal Syscalls to Implement

### 1. rt_sigaction - Signal Handler Setup
**Syscall Numbers**:
- x86_64: 13
- aarch64: 134

**Function Signature**:
```c
int rt_sigaction(int signum,
                 const struct sigaction *act,
                 struct sigaction *oldact,
                 size_t sigsetsize);
```

**Purpose**: Examine and change the action taken by a process on receipt of a specific signal.

**Key Data Structures**:
```c
struct sigaction {
    void     (*sa_handler)(int);              // Simple handler
    void     (*sa_sigaction)(int, siginfo_t *, void *);  // Advanced handler
    sigset_t   sa_mask;                       // Signals to block during handler
    int        sa_flags;                      // Modifier flags
    void     (*sa_restorer)(void);            // Return trampoline
};

typedef struct {
    int      si_signo;     // Signal number
    int      si_errno;     // Errno value
    int      si_code;      // Signal code
    pid_t    si_pid;       // Sending process ID
    uid_t    si_uid;       // Real user ID of sender
    union sigval si_value; // Signal value
    // ... more fields
} siginfo_t;
```

**Key Flags (sa_flags)**:
- **SA_SIGINFO**: Use 3-argument handler with siginfo_t
- **SA_NODEFER**: Don't block signal during handler execution
- **SA_ONSTACK**: Use alternate signal stack (sigaltstack)
- **SA_RESETHAND**: Reset to default on entry
- **SA_RESTART**: Restart interrupted system calls
- **SA_RESTORER**: Specify signal trampoline address

**Implementation Notes**:
- Cannot set handlers for SIGKILL (9) or SIGSTOP (19)
- Each signal has a default action: ignore, terminate, core dump, stop, or continue
- Signal handlers are per-process, not per-thread
- Need to store signal dispositions in process data structure

### 2. rt_sigprocmask - Signal Mask Manipulation
**Syscall Numbers**:
- x86_64: 14
- aarch64: 135

**Function Signature**:
```c
int rt_sigprocmask(int how,
                   const kernel_sigset_t *set,
                   kernel_sigset_t *oldset,
                   size_t sigsetsize);
```

**Purpose**: Examine and change blocked signals (signal mask) for the calling thread.

**Operations (how parameter)**:
- **SIG_BLOCK**: Add signals to current mask (union)
- **SIG_UNBLOCK**: Remove signals from current mask
- **SIG_SETMASK**: Replace current mask completely

**Implementation Notes**:
- Signal masks are per-thread (not per-process)
- sigset_t is architecture-specific size (larger than old 32-bit)
- Must block signal during handler execution (unless SA_NODEFER)
- Need to store signal mask in thread data structure

### 3. kill - Send Signal to Process
**Syscall Numbers**:
- x86_64: 62
- aarch64: 129

**Function Signature**:
```c
int kill(pid_t pid, int sig);
```

**Purpose**: Send signal to a process or process group.

**PID Behavior**:
- **pid > 0**: Send to process with ID `pid`
- **pid == 0**: Send to all processes in caller's process group
- **pid == -1**: Send to all processes caller has permission to signal (except init)
- **pid < -1**: Send to all processes in process group `-pid`

**Permission Checks**:
- Must have CAP_KILL capability, OR
- Real/effective UID must match target's real/saved-set UID

**Special Cases**:
- **sig == 0**: No signal sent, only permission/existence check
- Can't send SIGKILL (9) or SIGSTOP (19) to init (PID 1)

### 4. tgkill - Send Signal to Specific Thread
**Syscall Numbers**:
- x86_64: 234
- aarch64: 131

**Function Signature**:
```c
int tgkill(pid_t tgid, pid_t tid, int sig);
```

**Purpose**: Send signal to a specific thread within a specific thread group.

**Advantages over kill**:
- Targets specific thread (not arbitrary thread in process)
- Prevents race conditions with thread ID recycling
- Uses both thread group ID (tgid) and thread ID (tid)

**Implementation Notes**:
- tgid must match thread group of target thread
- More precise than kill for multithreaded processes
- Used internally by pthread libraries

### 5. sigaltstack - Alternate Signal Stack
**Syscall Numbers**:
- x86_64: 53
- aarch64: 132

**Function Signature**:
```c
int sigaltstack(const stack_t *ss, stack_t *old_ss);
```

**Purpose**: Define or query alternate signal stack for signal handlers.

**Key Data Structure**:
```c
typedef struct {
    void  *ss_sp;     // Base address of stack
    int    ss_flags;  // Flags (SS_DISABLE, SS_ONSTACK)
    size_t ss_size;   // Number of bytes in stack
} stack_t;
```

**Flags**:
- **SS_DISABLE**: Disable alternate signal stack
- **SS_ONSTACK**: Currently executing on alternate stack (read-only)
- **SS_AUTODISARM**: Auto-disable on entry (Linux 4.7+)

**Usage**:
1. Allocate memory for alternate stack (minimum SIGSTKSZ)
2. Call sigaltstack() to register it
3. Set SA_ONSTACK flag in sigaction()
4. Kernel automatically switches to alternate stack for signal delivery

**Constants**:
- **SIGSTKSZ**: Default size large enough for most handlers
- **MINSIGSTKSZ**: Minimum size required for signal handler

### 6. rt_sigreturn - Return from Signal Handler
**Syscall Numbers**:
- x86_64: 15
- aarch64: 139

**Function Signature**:
```c
int rt_sigreturn(unsigned long __unused);  // Architecture-specific
```

**Purpose**: Cleanup signal frame and restore process context after signal handler.

**Critical Behavior**:
- **Never returns** (restores context and resumes execution)
- Undoes everything done to invoke signal handler:
  - Restores signal mask
  - Switches back from alternate stack (if used)
  - Restores processor registers and flags
  - Restores stack pointer and instruction pointer

**Implementation Notes**:
- Called by signal trampoline code (in vdso or libc)
- Kernel creates signal frame on user stack before calling handler
- Signal frame contains: registers, signal mask, stack state
- Architecture-specific implementation (registers differ)
- Information saved in ucontext_t structure

## Signal Data Structures

### Signal Disposition Table (Per-Process)
```c
struct signal_disposition {
    void (*handler)(int);          // SIG_DFL, SIG_IGN, or function pointer
    sigset_t mask;                 // Signals to block during handler
    int flags;                     // SA_* flags
};

struct signal_disposition dispositions[NSIG];  // NSIG = 64 (standard + RT signals)
```

### Signal Pending Sets (Per-Process and Per-Thread)
```c
struct signal_state {
    sigset_t pending;              // Pending signals (process-wide)
    sigset_t blocked;              // Blocked signals (per-thread)
    struct sigqueue queue;         // Queued real-time signals
};
```

### Signal Frame (On User Stack)
```c
struct signal_frame {
    // Architecture-specific registers
    struct ucontext uc;
    
    // Signal info
    siginfo_t info;
    
    // Signal mask restoration
    sigset_t saved_mask;
    
    // Trampoline return address
    void *retcode;
};
```

## Signal Delivery Flow

### 1. Signal Generation (kill/tgkill)
```
User calls kill(pid, sig)
  → Check permissions (UID/CAP_KILL)
  → Find target process/thread
  → Add signal to pending set
  → If not blocked and thread can receive:
     → Queue signal (especially for RT signals)
     → Wake up target if sleeping
```

### 2. Signal Delivery (Kernel Decision)
```
Kernel returns to user mode
  → Check for pending unblocked signals
  → Select highest-priority signal (low number first)
  → If signal has handler:
     → Create signal frame on user stack
     → Save current context (registers, mask)
     → Update signal mask (block sa_mask + current signal)
     → Switch to alternate stack if SA_ONSTACK
     → Modify user context to call handler
  → If signal is SIG_DFL:
     → Take default action (terminate, core dump, ignore, etc.)
```

### 3. Signal Handler Execution
```
User mode signal handler runs
  → Handler processes signal
  → Handler returns (or calls longjmp)
```

### 4. Signal Return (rt_sigreturn)
```
Signal trampoline calls rt_sigreturn
  → Kernel validates signal frame
  → Restores signal mask (unblock blocked signals)
  → Restores alternate stack state
  → Restores registers and context
  → Resumes execution at interrupted point
```

## Standard Signals (1-31)

| Signal | Number | Default Action | Description |
|--------|--------|----------------|-------------|
| SIGHUP | 1 | Terminate | Hangup detected on controlling terminal |
| SIGINT | 2 | Terminate | Interrupt from keyboard (Ctrl+C) |
| SIGQUIT | 3 | Core dump | Quit from keyboard (Ctrl+\) |
| SIGILL | 4 | Core dump | Illegal Instruction |
| SIGABRT | 6 | Core dump | Abort signal from abort(3) |
| SIGFPE | 8 | Core dump | Floating point exception |
| SIGKILL | 9 | Terminate | Kill signal (cannot be caught/ignored) |
| SIGSEGV | 11 | Core dump | Invalid memory reference |
| SIGPIPE | 13 | Terminate | Broken pipe: write to pipe with no readers |
| SIGALRM | 14 | Terminate | Timer signal from alarm(2) |
| SIGTERM | 15 | Terminate | Termination signal |
| SIGCHLD | 17 | Ignore | Child stopped or terminated |
| SIGCONT | 18 | Continue | Continue if stopped |
| SIGSTOP | 19 | Stop | Stop process (cannot be caught/ignored) |
| SIGTSTP | 20 | Stop | Stop typed at terminal (Ctrl+Z) |

## Real-Time Signals (SIGRTMIN to SIGMAX)

**Range**: SIGRTMIN (34) to SIGRTMAX (64) on Linux

**Characteristics**:
- Guaranteed delivery order (multiple instances queued)
- Carry additional data (siginfo_t->si_value)
- Prioritized by signal number (lower = higher priority)
- Support for signal-specific data (int/pointer)

## Integration with Existing Systems

### Process Management
- Signal dispositions stored in linux_proc_append
- Signal pending set per process
- SIGCHLD handling for child process termination

### Thread Management
- Signal masks stored in linux_thread_append
- Per-thread signal pending sets
- tgkill for targeted signal delivery

### Memory Management
- Signal frame allocation on user stack
- Alternate signal stack management
- Signal queue for real-time signals

### Exit/Wait Mechanisms
- SIGCHLD generation on child exit
- Signal disposition for child termination
- Integration with wait4/waitpid

## Implementation Challenges

### 1. Architecture-Specific Signal Frames
- Different register sets between x86_64 and aarch64
- Different stack layouts and calling conventions
- Need architecture-specific signal frame setup/restore

### 2. Signal Mask Management
- Per-thread vs per-process signal masks
- Proper blocking/unblocking during handler execution
- Interaction with clone(CLONE_SIGHAND)

### 3. Real-Time Signal Queuing
- Multiple instances of same signal must be queued
- Need sigqueue data structure
- Priority ordering for signal delivery

### 4. Signal Handler Safety
- Only async-signal-safe functions can be called
- Reentrancy concerns for signal handlers
- Interaction with existing system calls

### 5. Alternate Stack Management
- Stack overflow detection
- Proper stack switching
- Nested signal handling

### 6. Signal Trampoline
- Need vdso or libc trampoline code
- Architecture-specific return sequence
- SA_RESTORER flag handling

## Testing Strategy

### Unit Tests
1. **Signal Handler Setup**: Test rt_sigaction with various flags
2. **Signal Mask Manipulation**: Test rt_sigprocmask operations
3. **Signal Sending**: Test kill and tgkill with different PID values
4. **Alternate Stack**: Test sigaltstack setup and usage
5. **Signal Return**: Test rt_sigreturn behavior

### Integration Tests
1. **SIGCHLD Handling**: Test child process notification
2. **Signal Delivery Order**: Test real-time signal queuing
3. **Multi-threaded Signals**: Test signal delivery to threads
4. **Signal Safety**: Test async-signal-safe function calls
5. **Stack Overflow**: Test alternate stack usage

### Architecture Tests
1. **x86_64**: Test all signal syscalls on x86_64
2. **aarch64**: Test all signal syscalls on aarch64
3. **Signal Frame Layout**: Verify architecture-specific frames
4. **Register Restoration**: Test context restoration accuracy

## Next Steps

1. **Implement Basic Signal Infrastructure**:
   - Signal disposition storage in linux_proc_append
   - Signal mask storage in linux_thread_append
   - Signal pending sets (process and thread)
   - Signal queue for real-time signals

2. **Implement rt_sigaction**:
   - Signal handler registration
   - Flag validation and processing
   - Signal disposition storage

3. **Implement rt_sigprocmask**:
   - Signal mask manipulation
   - Per-thread mask management

4. **Implement kill/tgkill**:
   - Signal generation
   - Permission checking
   - Target process/thread lookup

5. **Implement Signal Delivery**:
   - Signal frame creation (architecture-specific)
   - Context saving and restoration
   - Handler invocation

6. **Implement sigaltstack**:
   - Alternate stack management
   - Stack switching logic

7. **Implement rt_sigreturn**:
   - Signal frame cleanup
   - Context restoration
   - Architecture-specific implementation

## Conclusion

Phase 2B is a critical phase that implements the signal mechanism, which is fundamental to Linux process communication and exception handling. The signal mechanism is complex and requires careful implementation to ensure correctness and compatibility with Linux signal semantics.

The implementation will require:
- Architecture-specific signal frame handling
- Per-process and per-thread signal state management
- Integration with existing process and thread management
- Careful attention to signal safety and reentrancy

**Phase 2B Status**: 🔍 **ANALYSIS COMPLETE, READY FOR IMPLEMENTATION**