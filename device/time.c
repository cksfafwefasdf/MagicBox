#include <time.h>
#include <io.h>
#include <interrupt.h>
#include <vgacon.h>
#include <timer.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio-kernel.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

// CMOS 寄存器索引
#define SECONDS          0x00
#define MINUTES          0x02
#define HOURS            0x04
#define DAY_OF_MONTH     0x07
#define MONTH            0x08
#define YEAR             0x09
#define STATUS_REG_A     0x0A
#define STATUS_REG_B     0x0B

static int64_t startup_time; // 系统启动时的 Unix 时间戳

static bool is_updating(void); 

// 检查 RTC 是否正在更新（此时读取数据可能不准确）
static bool is_updating() {
    outb(CMOS_ADDR, STATUS_REG_A);
    return (inb(CMOS_DATA) & 0x80);
}

static uint8_t get_rtc_register(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

// BCD 码转二进制，我们从寄存器组中读出来的是BCD码，要转成二进制
#define BCD_TO_BIN(val) (((val) & 0x0F) + ((val) >> 4) * 10)

static int month_days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static int64_t mktime(struct tm *t) {
    int64_t res = 0;
    int year = t->tm_year;

    // 累加从 1970 到当前年份之前的整年秒数
    for (int y = 1970; y < year; y++) {
        res += (365 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) * 86400LL;
    }

    // 累加当年之前月份的秒数 (t->tm_mon 是 0-11)
    for (int m = 1; m <= t->tm_mon; m++) {
        res += month_days[m] * 86400LL;
        // 如果是当年且是闰年，且过了2月，补一天
        if (m == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
            res += 86400LL;
        }
    }

    // 累加当月的日期、小时、分钟、秒
    res += (t->tm_mday - 1) * 86400LL;
    res += t->tm_hour * 3600LL;
    res += t->tm_min * 60LL;
    res += t->tm_sec;

    return res;
}

void time_init(void) {
    struct tm t;
    
    // 等待更新周期结束，确保读到准确值
    while (is_updating());

    t.tm_sec  = BCD_TO_BIN(get_rtc_register(SECONDS));
    t.tm_min  = BCD_TO_BIN(get_rtc_register(MINUTES));
    t.tm_hour = BCD_TO_BIN(get_rtc_register(HOURS));
    t.tm_mday = BCD_TO_BIN(get_rtc_register(DAY_OF_MONTH));
    // t.tm_mon  = BCD_TO_BIN(get_rtc_register(MONTH));
    // 将 tm_mon 减 1，使其从0开始，与c库对齐
    t.tm_mon  = BCD_TO_BIN(get_rtc_register(MONTH))-1;
    // 加上 2000 年（RTC 的 YEAR 通常只存后两位，如 26 表示 2026）
    t.tm_year = BCD_TO_BIN(get_rtc_register(YEAR)) + 2000;

    startup_time = mktime(&t);
    
    // 打印一下看看对不对（比如北京/新加坡时间 +8）
    printk("System start time: %d-%d-%d %d:%d:%d UTC\n", 
            t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

// 供 sys_stat 或其他系统调用获取当前时间
int64_t sys_time(void) {
    // 强制让除号两边都是 32 位类型
    uint32_t t = (uint32_t)ticks;
    uint32_t f = (uint32_t)IRQ0_FREQUENCY;
    // 这样编译器会生成 'div' 指令（32位除法），而不是去调用 '__divdi3'，防止链接报错
    return startup_time + (int64_t)(t / f);
}