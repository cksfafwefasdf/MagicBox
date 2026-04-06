#ifndef __INCLUDE_MAGICBOX_SYSCALL_INTRCEPT
#define __INCLUDE_MAGICBOX_SYSCALL_INTRCEPT

struct intr_stack;

extern void musl_syscall_interceptor(struct intr_stack* stack);
extern void musl_syscall_intrcpt_init(void);
#endif