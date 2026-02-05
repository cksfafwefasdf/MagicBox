#ifndef __DEVICE__BLOCK_DEV_H
#define __DEVICE__BLOCK_DEV_H

#include "stdint.h"
struct file_operations;

#define MAX_BLOCK_DEVS 8

struct block_device {
    char* name;
    struct file_operations* fops;
};

extern void register_block_dev(uint8_t major, struct file_operations* fops, char* name);
extern struct file_operations* get_block_dev_fops(uint8_t major);

#endif