#ifndef __LIB_KERNEL_PRINT_H
#define __LIB_KERNEL_PRINT_H
#include "stdint.h"
// if dont has extern
// gcc will assume it is extern by default
extern void put_char(uint8_t char_ascii);
extern void put_str(char *str);
extern void put_int(uint32_t num); // print as 16based
extern void set_cursor(uint16_t pos);
extern void cls_screen(void);
#endif
