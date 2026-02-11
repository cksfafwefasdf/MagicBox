#ifndef __DEVICE_TIMER_H
#define __DEVICE_TIMER_H
#include "stdint.h"

extern void timer_init(void);
// sleep is measured in mil-second
extern void mtime_sleep(uint32_t m_seconds);

extern int sys_pause(void);
extern uint32_t sys_alarm(uint32_t seconds);
#endif