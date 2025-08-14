#include "sync.h"
#include "interrupt.h"
#include "debug.h"

void sema_init(struct semaphore* psema,uint8_t value){
	psema->value = value;
	dlist_init(&psema->waiters);
}

void lock_init(struct lock* plock){
	plock->holder = NULL;
	plock->holder_repeat_nr = 0;
	sema_init(&plock->semaphore,1);
}

void sema_wait(struct semaphore* psema){
	enum intr_status old_stat = intr_disable();
	while(psema->value==0){
		ASSERT(!dlist_find(&psema->waiters,&get_running_task_struct()->general_tag));
		if(dlist_find(&psema->waiters,&get_running_task_struct()->general_tag)){
			PANIC("sema_wait: thread blocked has been in waiter_list\n");
		}
		dlist_push_back(&psema->waiters,&get_running_task_struct()->general_tag);
		thread_block(TASK_BLOCKED);
	}
	psema->value--;
	ASSERT(psema->value==0);
	intr_set_status(old_stat);
}

void sema_signal(struct semaphore* psema){
	enum intr_status old_stat = intr_disable();
	ASSERT(psema->value==0);
	if(!dlist_empty(&psema->waiters)){
		struct task_struct* thread_blocked = elem2entry(struct task_struct,dlist_pop_front(&psema->waiters));
		thread_unblock(thread_blocked);
	}
	psema->value++;
	ASSERT(psema->value==1);
	intr_set_status(old_stat);
}

void lock_acquire(struct lock* plock){
	if(plock->holder != get_running_task_struct()){
		sema_wait(&plock->semaphore);
		plock->holder = get_running_task_struct();
		ASSERT(plock->holder_repeat_nr==0);
		plock->holder_repeat_nr = 1;
	}else{
		plock->holder_repeat_nr++;
	}
}

void lock_release(struct lock* plock){
	ASSERT(plock->holder==get_running_task_struct());
	if(plock->holder_repeat_nr>1){
		plock->holder_repeat_nr--;
		return;
	}
	ASSERT(plock->holder_repeat_nr==1);
	plock->holder = NULL;
	plock->holder_repeat_nr = 0;
	sema_signal(&plock->semaphore);
}