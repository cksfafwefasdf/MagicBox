#include "syscall.h"

#define _syscall0(SYSCALL_NUM) ({ \
	int retval; \
	asm volatile( \
	"pushl %[number];int $0x80;addl $4,%%esp" \
	:"=a"(retval) \
	:[number] "i" (SYSCALL_NUM) \
	:"memory" \
	); \
	retval; \
})

#define _syscall3(SYSCALL_NUM,ARG0,ARG1,ARG2) ({ \
	int retval; \
	asm volatile( \
		"pushl %[arg2];pushl %[arg1];pushl %[arg0]; \ 
		pushl %[number];int $0x80;addl $16,%%esp" \
		:"=a"(retval) \
		:[number]"i"(SYSCALL_NUM), \
		 [arg0] "g" (ARG0), \
		 [arg1] "g" (ARG1), \
		 [arg2] "g" (ARG2), \
		:"memory" \
	); \
	retval; \
})

uint32_t getpid(void){
	return _syscall0(SYS_GETPID);
}