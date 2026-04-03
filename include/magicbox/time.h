#ifndef __INCLUDE_MAGICBOX_TIME_H
#define __INCLUDE_MAGICBOX_TIME_H

#include <stdint.h>

struct tm {
    int tm_sec;   // 0-59
    int tm_min;   // 0-59
    int tm_hour;  // 0-23
    int tm_mday;  // 1-31
    int tm_mon;   // 1-12
    int tm_year;  // 实际年份，如 2026
};

extern void time_init(void);
extern int64_t sys_time(void); // 返回当前 Unix 时间戳

#endif