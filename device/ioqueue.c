#include "ioqueue.h"
#include "interrupt.h"
#include "global.h"
#include "debug.h"
#include "stdint.h"
#include "stdbool.h"

void ioqueue_init(struct ioqueue* ioq){
	lock_init(&ioq->lock);
	ioq->producer = ioq->consumer = NULL;
	ioq->head = ioq->tail = 0;
}

static int32_t next_pos(int32_t pos){
	return (pos+1)%BUFSIZE;
}

bool ioq_full(struct ioqueue* ioq){
	ASSERT(intr_get_status()==INTR_OFF);
	return next_pos(ioq->head) == ioq->tail;
}

bool ioq_empty(struct ioqueue* ioq){
	ASSERT(intr_get_status()==INTR_OFF);
	return ioq->head==ioq->tail;
}

void ioq_wait(struct task_struct** waiter){
	ASSERT(*waiter==NULL&&waiter!=NULL);
	*waiter = get_running_task_struct();
	thread_block(TASK_BLOCKED);
}

void ioq_wakeup(struct task_struct** waiter){
	ASSERT(*waiter!=NULL);
	thread_unblock(*waiter);
	*waiter = NULL;
}

char ioq_getchar(struct ioqueue* ioq){
	ASSERT(intr_get_status()==INTR_OFF);

	while(ioq_empty(ioq)){
		lock_acquire(&ioq->lock);
		ioq_wait(&ioq->consumer);
		lock_release(&ioq->lock);
	}

	char byte = ioq->buf[ioq->tail];
	ioq->tail = next_pos(ioq->tail);
	if(ioq->producer!=NULL){
		ioq_wakeup(&ioq->producer);
	}
	return byte;
}

// 非阻塞地从队列取一个字符
// 非阻塞模式不要拿 ioq->lock ，防止死锁
char ioq_get_raw(struct ioqueue* ioq) {
    ASSERT(intr_get_status() == INTR_OFF);
    ASSERT(!ioq_empty(ioq)); // 调用前必须保证不空

    char byte = ioq->buf[ioq->tail];
    ioq->tail = next_pos(ioq->tail);

    // 腾出了空间，如果有生产者在等，顺手唤醒
    if (ioq->producer != NULL) {
        ioq_wakeup(&ioq->producer);
    }
    return byte;
}

// 非阻塞地往队列放一个字符
void ioq_put_raw(struct ioqueue* ioq, char byte) {
    ASSERT(intr_get_status() == INTR_OFF);
    ASSERT(!ioq_full(ioq)); // 调用前必须保证不满

    ioq->buf[ioq->head] = byte;
    ioq->head = next_pos(ioq->head);

    // 有了新数据，如果有消费者在等，顺手唤醒
    if (ioq->consumer != NULL) {
        ioq_wakeup(&ioq->consumer);
    }
}

// 返回 ioqueue 中最后一个存入的字符（即 head 的前一个字符）
char ioq_last_char(struct ioqueue* ioq) {
    ASSERT(intr_get_status() == INTR_OFF);
    
    // 如果队列为空，直接报错或返回空字符（理论上调用前应先 check ioq_empty）
    if (ioq_empty(ioq)) {
        return '\0';
    }

    // head 指向的是下一个空位，所以要减 1
    // 加上 BUFSIZE 再取模是为了处理 head 为 0 时回绕到缓冲区末尾的情况
    uint32_t last_pos = (ioq->head - 1 + BUFSIZE) % BUFSIZE;
    
    return ioq->buf[last_pos];
}

// 从ioqueue的头部删除数据
void ioq_popchar(struct ioqueue* ioq) {
    ASSERT(intr_get_status() == INTR_OFF);
    if (!ioq_empty(ioq)) {
        // head 指针回退一格 (处理环形边界)
        ioq->head = (ioq->head - 1 + BUFSIZE) % BUFSIZE;
        
        // 如果之前有生产者因为缓冲区满而阻塞，现在有空间了，可以唤醒它
        if (ioq->producer != NULL) {
            ioq_wakeup(&ioq->producer);
        }
    }
}

void ioq_putchar(struct ioqueue* ioq,char byte){
	ASSERT(intr_get_status()==INTR_OFF);

	while(ioq_full(ioq)){
		lock_acquire(&ioq->lock);
		ioq_wait(&ioq->producer);
		lock_release(&ioq->lock);
	}
	ioq->buf[ioq->head] = byte;
	ioq->head = next_pos(ioq->head);

	if(ioq->consumer!=NULL){
		ioq_wakeup(&ioq->consumer);
	}

}

uint32_t ioq_length(struct ioqueue* ioq){
	uint32_t len = 0;
	if(ioq->head>=ioq->tail){
		len = ioq->head - ioq->tail;
	}else{
		len = BUFSIZE - (ioq->tail-ioq->head);
	}
	return len;
}
