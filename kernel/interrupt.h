#ifndef __KERNEL__INTERRUPT_H
#define __KERNEL__INTERRUPT_H
#include "stdint.h"


#define SCREEN_POS(row,col) (row*NUM_FULL_LINE_CH+col) 

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