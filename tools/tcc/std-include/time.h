/* time.h */
#ifndef _TIME_H
#define _TIME_H
#include <stdint.h>
#include <sys/types.h>
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};
struct tm *localtime(const time_t *timep);
struct tm *gmtime(const time_t *timep);
time_t time(time_t *t);
struct timeval { long tv_sec; long tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };
int gettimeofday(struct timeval *tv, struct timezone *tz);
/* TanjaOS timing (kernel exports) */
uint32_t get_uptime_ms(void);
void timer_delay_ms(uint32_t ms);
#endif
