#ifndef _LINUX_COMPAT_TIME_TYPES_H_
#define _LINUX_COMPAT_TIME_TYPES_H_

#include <common/types.h>

typedef i64 linux_time_t;
typedef i64 linux_suseconds_t;
typedef i64 linux_clock_t;

typedef struct linux_timeval {
        linux_time_t tv_sec;
        linux_suseconds_t tv_usec;
} linux_timeval_t;

typedef struct linux_timespec {
        linux_time_t tv_sec;
        i64 tv_nsec;
} linux_timespec_t;

typedef struct linux_timezone {
        i32 tz_minuteswest;
        i32 tz_dsttime;
} linux_timezone_t;

typedef struct linux_tms {
        linux_clock_t tms_utime;
        linux_clock_t tms_stime;
        linux_clock_t tms_cutime;
        linux_clock_t tms_cstime;
} linux_tms_t;

#define LINUX_UTS_NAME_LEN 65

typedef struct linux_utsname {
        char sysname[LINUX_UTS_NAME_LEN];
        char nodename[LINUX_UTS_NAME_LEN];
        char release[LINUX_UTS_NAME_LEN];
        char version[LINUX_UTS_NAME_LEN];
        char machine[LINUX_UTS_NAME_LEN];
        char domainname[LINUX_UTS_NAME_LEN];
} linux_utsname_t;

#define LINUX_CLOCK_REALTIME           0
#define LINUX_CLOCK_MONOTONIC          1
#define LINUX_CLOCK_PROCESS_CPUTIME_ID 2
#define LINUX_CLOCK_THREAD_CPUTIME_ID  3

#endif
