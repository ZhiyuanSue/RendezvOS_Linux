#ifndef _LINUX_COMPAT_FS_LINUX_POLL_H_
#define _LINUX_COMPAT_FS_LINUX_POLL_H_

#include <common/types.h>

/*
 * Minimal poll(2) / ppoll(2) constants and pollfd layout for BusyBox ash.
 * Event bit values match Linux uapi on x86_64 and aarch64.
 */

#define LINUX_POLLIN   0x0001
#define LINUX_POLLPRI  0x0002
#define LINUX_POLLOUT  0x0004
#define LINUX_POLLERR  0x0008
#define LINUX_POLLHUP  0x0010
#define LINUX_POLLNVAL 0x0020

#define LINUX_POLL_MAX_NFDS 64u

typedef struct {
        i32 fd;
        i16 events;
        i16 revents;
} linux_pollfd_t;

#endif /* _LINUX_COMPAT_FS_LINUX_POLL_H_ */
