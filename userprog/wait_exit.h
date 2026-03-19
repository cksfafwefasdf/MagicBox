#ifndef __USERPROG_WAIT_EXIT_H
#define __USERPROG_WAIT_EXIT_H
#include "../lib/stdint.h"
#include "../thread/thread.h"

extern pid_t sys_wait(int32_t* status);
extern void sys_exit(int32_t status);

#endif