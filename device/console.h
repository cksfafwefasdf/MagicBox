#ifndef __DEVICE_CONSOLE_H
#define __DEVICE_CONSOLE_H
#include "../lib/stdint.h"

extern void console_init(void);
extern void console_acquire(void);
extern void console_release(void);
extern void console_put_char(uint8_t char_asci);
extern void console_put_str(char* str);
extern void console_put_int_HAX(uint32_t num);
#endif