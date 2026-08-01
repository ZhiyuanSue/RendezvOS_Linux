#ifndef _LINUX_COMPAT_FS_LINUX_FCNTL_H_
#define _LINUX_COMPAT_FS_LINUX_FCNTL_H_

/*
 * Minimal fcntl(2) / open(2) flag constants for ash / BusyBox.
 *
 * Cmd numbers F_DUPFD..F_SETFL and F_DUPFD_CLOEXEC are the same on
 * x86_64 and aarch64 Linux ABIs. Syscall *numbers* differ (__NR_fcntl
 * is 72 on x86_64, 25 on aarch64) and are selected via arch syscall_ids.h.
 *
 * O_CLOEXEC / O_NONBLOCK / O_APPEND / O_CREAT bit values also match both
 * ABIs. O_DIRECTORY differs on aarch64 — see LINUX_O_DIRECTORY_AARCH64
 * and linux_open_flags_normalize() in sys_fs_impl.c.
 */

#define LINUX_F_DUPFD            0
#define LINUX_F_GETFD            1
#define LINUX_F_SETFD            2
#define LINUX_F_GETFL            3
#define LINUX_F_SETFL            4
#define LINUX_F_DUPFD_CLOEXEC    1030

#define LINUX_FD_CLOEXEC         1

#define LINUX_O_ACCMODE          0003
#define LINUX_O_RDONLY           00
#define LINUX_O_WRONLY           01
#define LINUX_O_RDWR             02
#define LINUX_O_CREAT            0100
#define LINUX_O_APPEND           02000
#define LINUX_O_NONBLOCK         04000
#define LINUX_O_DIRECTORY        0200000
#define LINUX_O_CLOEXEC          02000000

#if defined(_AARCH64_)
/* aarch64 Linux UAPI uses a different O_DIRECTORY bit. */
#define LINUX_O_DIRECTORY_AARCH64 040000
#endif

/* Status flags F_SETFL may change (subset; enough for BusyBox). */
#define LINUX_F_SETFL_MASK       (LINUX_O_APPEND | LINUX_O_NONBLOCK)

#endif /* _LINUX_COMPAT_FS_LINUX_FCNTL_H_ */
