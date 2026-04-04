#ifndef __INCLUDE_MAGICBOX_UART_H
#define __INCLUDE_MAGICBOX_UART_H

#include <stdbool.h>

extern bool is_uart_present(void);
extern char uart_getc(void);
extern int serial_received(void);
extern void uart_puts(const char* s);
extern void uart_putc(char a);
extern int is_transmit_empty(void);
extern void uart_init(void);

#endif