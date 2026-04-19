#ifndef __INCLUDE_MAGICBOX_INTERRUPT_H
#define __INCLUDE_MAGICBOX_INTERRUPT_H
#include <stdint.h>


#define SCREEN_POS(row,col) (row*SCREEN_WIDTH+col) 

// 此中断号除了 syscall.c 会用外，start.s 里面的 sigreturn 也会用
#define NATIVE_SYSCALL_NR 0x77

typedef void* intr_handler_addr;

enum intr_status{
    INTR_OFF,
    INTR_ON
};

extern void intr_init(void);

// the return is old_status
extern enum intr_status intr_enable(void);
extern enum intr_status intr_disable(void);
extern enum intr_status intr_set_status(enum intr_status status);
extern enum intr_status intr_get_status(void);
extern void register_handler(uint8_t vec_no,intr_handler_addr function);
#endif