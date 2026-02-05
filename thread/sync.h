#ifndef __THREAD_SYNC_H
#define __THREAD_SYNC_H
#include "dlist.h"
#include "stdint.h"
#include "thread.h"

struct semaphore{
	uint8_t value;
	struct dlist waiters;
};

struct lock{
	struct task_struct* holder;
	struct semaphore semaphore;
	uint32_t holder_repeat_nr;
};

extern void sema_init(struct semaphore* psema,uint8_t value);
extern void lock_init(struct lock* plock);
extern void sema_wait(struct semaphore* psema);
extern void sema_signal(struct semaphore* psema);
extern void lock_acquire(struct lock* plock);
extern void lock_release(struct lock* plock);
#endif