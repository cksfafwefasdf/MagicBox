#ifndef __USERPROG_SYSCALLINIT_H
#define __USERPROG_SYSCALLINIT_H
// syscall-init.h includes interfaces for OS
#include "../lib/stdint.h"

extern void syscall_init(void);
extern uint32_t sys_getpid(void);
// extern uint32_t sys_write(char* str);
// extern uint32_t sys_write_int(int num);
#endif