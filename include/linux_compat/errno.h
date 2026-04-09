#ifndef _RENDEZVOS_LINUX_COMPAT_ERRNO_H_
#define _RENDEZVOS_LINUX_COMPAT_ERRNO_H_

/*
 * Linux compatibility errno (user-visible).
 *
 * Do NOT return RendezvOS internal errors (e.g. -E_RENDEZVOS == -1024) from
 * linux_layer syscalls to userspace: they are intentionally outside Linux
 * errno space and will confuse libc/tests.
 *
 * Keep this file minimal; extend as needed per implemented syscalls.
 */

#define LINUX_EPERM   1
#define LINUX_ENOENT  2
#define LINUX_EINTR   4
#define LINUX_EIO     5
#define LINUX_ENXIO   6
#define LINUX_E2BIG   7
#define LINUX_ENOEXEC 8
#define LINUX_EBADF   9
#define LINUX_ECHILD  10
#define LINUX_EAGAIN  11
#define LINUX_ENOMEM  12
#define LINUX_EACCES  13
#define LINUX_EFAULT  14
#define LINUX_EBUSY   16
#define LINUX_EEXIST  17
#define LINUX_EXDEV   18
#define LINUX_ENODEV  19
#define LINUX_ENOTDIR 20
#define LINUX_EISDIR  21
#define LINUX_EINVAL  22
#define LINUX_ENFILE  23
#define LINUX_EMFILE  24
#define LINUX_ENOTTY  25
#define LINUX_EFBIG   27
#define LINUX_ENOSPC  28
#define LINUX_ESPIPE  29
#define LINUX_EROFS   30
#define LINUX_EMLINK  31
#define LINUX_EPIPE   32
#define LINUX_ERANGE  34

#define LINUX_ESRCH   3
#define LINUX_ENOSYS  38

#endif

