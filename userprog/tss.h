#ifndef __USERPROG_TSS_H
#define __USERPROG_TSS_H
#include "../thread/thread.h"
extern void update_tss_esp(struct task_struct* pthread);
extern void tss_init(void);

#endif