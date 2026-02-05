#ifndef __DEVICE__CHAR_DEV_H
#define __DEVICE__CHAR_DEV_H
#include "stdint.h"

struct file_operations;

#define MAX_CHAR_DEVS 8

struct char_device {
    char* name;
    struct file_operations* fops;
};

extern void register_char_dev(uint8_t major, struct file_operations* fops, char* name);
extern struct file_operations* get_char_dev_fops(uint8_t major);

#endif