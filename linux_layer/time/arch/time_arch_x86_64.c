#if defined(_X86_64_)

#include <arch/x86_64/time.h>
#include <linux_compat/time/linux_time_arch.h>

static bool linux_time_is_leap(u16 year)
{
        return (year % 4u == 0u && year % 100u != 0u) || (year % 400u == 0u);
}

static u64 linux_rtc_to_unix_sec(const struct rtc_time *rtc)
{
        static const u16 days_in_month[] = {
                31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        u64 days = 0;
        u16 y;
        u16 m;

        if (!rtc || rtc->year < 1970 || rtc->month < 1 || rtc->month > 12
            || rtc->day < 1 || rtc->day > 31) {
                return 0;
        }

        for (y = 1970; y < rtc->year; y++) {
                days += linux_time_is_leap(y) ? 366u : 365u;
        }
        for (m = 1; m < rtc->month; m++) {
                days += days_in_month[m - 1];
                if (m == 2 && linux_time_is_leap(rtc->year)) {
                        days += 1;
                }
        }
        days += (u64)rtc->day - 1u;
        return days * 86400ull + (u64)rtc->hour * 3600ull
               + (u64)rtc->min * 60ull + (u64)rtc->sec;
}

u64 linux_time_arch_boot_realtime_us(void)
{
        struct rtc_time rtc = get_rtc_time();

        return linux_rtc_to_unix_sec(&rtc) * 1000000ull;
}

#endif /* _X86_64_ */
