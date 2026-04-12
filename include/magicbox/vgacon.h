#ifndef __INCLUDE_ARCH_PRINT_H
#define __INCLUDE_ARCH_PRINT_H
#include <stdint.h>
#include <device.h>

// vga 控制台相关的操作函数

// if dont has extern
// gcc will assume it is extern by default
extern void put_char(char char_ascii);
extern void put_str(const char *str);
extern void put_int(uint32_t num); // print as 16based
extern void set_cursor(uint16_t pos);
extern void cls_screen(void);
extern void vgacon_init(void);

extern struct console_device console_vgacon;
#endif
