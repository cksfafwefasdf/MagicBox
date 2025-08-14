#ifndef __DEVICE_IOQUEUE_H
#define __DEVICE_IOQUEUE_H

#include "../thread/sync.h"
#include "../lib/stdint.h"

#define BUFSIZE 2048

struct ioqueue{
	struct lock lock;
	struct task_struct* producer;
	struct task_struct* consumer;
	char buf[BUFSIZE];
	int32_t head;
	int32_t tail; 
};


extern void ioqueue_init(struct ioqueue* ioq);
extern bool ioq_full(struct ioqueue* ioq);
extern char ioq_getchar(struct ioqueue* ioq);
extern void ioq_putchar(struct ioqueue* ioq,char byte);
extern bool ioq_empty(struct ioqueue* ioq);
extern uint32_t ioq_length(struct ioqueue* ioq);

#endif