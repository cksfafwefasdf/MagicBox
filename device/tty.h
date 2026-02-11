#ifndef __DEVICE_TTY_H
#define __DEVICE_TTY_H

#include "stdint.h"
#include "stdbool.h"
#include "sync.h"
#include "ioqueue.h"
#include "fs_types.h"
#include "termios.h"


struct tty_struct {
	struct termios termios;
	struct ioqueue ibuf; // 原始输入队列 (相当于 Linux 的 read_q)
	struct ioqueue obuf;
    struct semaphore line_sem; // 专门用于按行同步的信号量
	// 由于我们在 tty 上已经套了一把锁了
	// 因此我们在 tty 里面的操作中，就没有必要再去调用 console_put_char 了，直接调用 put_char 函数就行
	// 但是这个 console_put_char 还是得留着，因为我们往 tty 写数据时，会经过 tty 的缓冲区
	// 但是有时候内核调试我们并不需要经过 tty 的缓冲区，因为内核中没有什么代码会来取这个缓冲区
	// 这个缓冲区很多情况下都是 shell 来取的，如果我们在内核调试时也用 tty_read 一类的函数
	// 那么缓冲区的数据就没人取了！console_put_char 主要给 printk 啥的用

	// 将内部的所有 put_char 都改成 console_put_char
	// 这样的话这整个 tty 应该就不用加额外的锁了
	// 因为 console_put_char 本身就是带锁的，ioqueue 也是带锁的，所以应该不用再添加额外的锁了
	// 但是最好还是预留在这
	struct lock tty_lock; // 用于防止两个进程同时对tty进行写操作，也就是说这是个写锁，line_sem是个读锁
	int32_t pgrp; // 记录前台进程组id
};



extern struct tty_struct console_tty;
// TTY 的操作集
extern struct file_operations tty_dev_fops;

extern int tty_read(char* buf, uint32_t count);
extern void tty_init(void);
extern void tty_input_handler(char c);
extern int tty_write(const char* buf, uint32_t count);
extern void tty_dev_init(void);
extern int32_t tty_dev_read(struct file* file, void* buf, uint32_t count);
extern int32_t tty_dev_write(struct file* file, const void* buf, uint32_t count);
extern int32_t sys_ioctl(int fd, uint32_t cmd, uint32_t arg);
extern int32_t tty_ioctl(struct tty_struct* tty, uint32_t cmd, uint32_t arg); 

#endif