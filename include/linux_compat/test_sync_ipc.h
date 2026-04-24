#ifndef _LINUX_COMPAT_TEST_SYNC_IPC_H_
#define _LINUX_COMPAT_TEST_SYNC_IPC_H_

#include <common/types.h>
#include <common/string.h>

/*
 * Linux-compat test synchronization over kmsg/IPC:
 * - sys_exit() asks clean_server to send an ACK to a per-CPU reply port
 * - user_test_runner blocks on recv_msg(reply_port) to implement wait semantics
 */

#define LINUX_EXIT_REPLY_PORT_PREFIX "linux_exit_ack_"

/* Request: (thread_ptr, exit_code, reply_port_name, cookie) */
#define LINUX_KMSG_FMT_THREAD_REAP "p q t q"

/* Reply: (cookie, exit_code) */
#define LINUX_KMSG_FMT_EXIT_ACK "q q"

/* clean_server -> runner ACK opcode (module is reply port service_id). */
#define KMSG_OP_CORE_THREAD_REAP_ACK 2u

/*
 * Build per-CPU reply port name into `buf`.
 * Returns false if the buffer is too small.
 */
static inline bool linux_exit_ack_port_name(char* buf, u32 cap, u64 cpu)
{
        if (!buf || cap == 0)
                return false;
        /* prefix */
        const char* pfx = LINUX_EXIT_REPLY_PORT_PREFIX;
        u32 pfx_len = (u32)strlen(pfx);
        if (pfx_len + 2 > cap) { /* at least one digit + NUL */
                buf[0] = '\0';
                return false;
        }
        memcpy(buf, pfx, pfx_len);

        /* append decimal cpu */
        char tmp[24];
        u32 n = 0;
        if (cpu == 0) {
                tmp[n++] = '0';
        } else {
                while (cpu && n < (u32)sizeof(tmp)) {
                        tmp[n++] = (char)('0' + (cpu % 10));
                        cpu /= 10;
                }
        }
        if (pfx_len + n + 1 > cap) {
            buf[0] = '\0';
            return false;
        }
        /* reverse digits into buf */
        for (u32 i = 0; i < n; i++) {
                buf[pfx_len + i] = tmp[n - 1 - i];
        }
        buf[pfx_len + n] = '\0';
        return true;
}

#endif

