#ifndef __INCLUDE_MAGICBOX_TIMER_H
#define __INCLUDE_MAGICBOX_TIMER_H
#include <stdint.h>

#define IRQ0_FREQUENCY 100 //intr freq is 100 times/s
#define INPUT_FREQUENCY 1193180
#define COUNT0_INIT_COUNT_VALUE INPUT_FREQUENCY/IRQ0_FREQUENCY
#define COUNT0_PORT 0x40 
#define COUNT0_NO 0 // select counter0
#define COUNT0_MODE 2 // Rate Generator mode,software start
#define READ_WRITE_LATCH_MODE 3 // read/write low 8 bit at first,then do it again for high 8 bit
#define PIT_CONTROL_PORT 0x43 
#define IS_BCD 0 // BCD or binary 

#define mil_seconds_per_intr (1000/IRQ0_FREQUENCY)

extern void timer_init(void);
// sleep is measured in mil-second
extern void mtime_sleep(uint32_t m_seconds);

extern int sys_pause(void);
extern uint32_t sys_alarm(uint32_t seconds);

extern uint32_t ticks;
#endif