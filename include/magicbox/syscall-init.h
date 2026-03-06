#ifndef __INCLUDE_MAGICBOX_SYSCALLINIT_H
#define __INCLUDE_MAGICBOX_SYSCALLINIT_H
// syscall-init.h includes interfaces for OS
#include <stdint.h>

extern void syscall_init(void);
extern uint32_t sys_getpid(void);
#endif