#ifndef __INCLUDE_MAGICBOX_FIFO_H
#define __INCLUDE_MAGICBOX_FIFO_H

#include "stdint.h"

struct inode;
struct file;

extern int32_t fifo_open(struct inode* inode, struct file* file);
extern int32_t fifo_write(struct file* file, void* buf, uint32_t count);
extern int32_t fifo_read(struct file* file, void* buf, uint32_t count);
extern int32_t fifo_release(struct file* file);
#endif