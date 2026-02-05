#ifndef __DEVICE_CONSOLE_H
#define __DEVICE_CONSOLE_H
#include "stdint.h"

struct file;

extern struct file_operations console_dev_fops;

extern void console_init(void);
extern void console_acquire(void);
extern void console_release(void);
extern void console_put_char(uint8_t char_asci);
extern void console_put_str(char* str);
extern void console_put_int_HAX(uint32_t num);
extern int32_t console_dev_write(struct file* file, const void* buf, uint32_t count);
#endif