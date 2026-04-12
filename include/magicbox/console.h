#ifndef __INCLUDE_MAGICBOX_CONSOLE_H
#define __INCLUDE_MAGICBOX_CONSOLE_H
#include <stdint.h>
#include <dlist.h>

#define CONSOLE_DEV_NAME_LEN 16
#define BROADCAST_RDEV 0xffffffff

struct file;

// console.h
struct console_device {
    void (*put_char)(char char_asci);
    void (*put_str)(const char* str);
    void (*put_int)(uint32_t num);
    uint32_t rdev; // 设备id
    char name[CONSOLE_DEV_NAME_LEN];
    struct dlist_elem dev_tag;
};

extern struct file_operations console_dev_fops;

extern void console_init(void);
extern void console_acquire(void);
extern void console_release(void);
extern void console_put_char(uint8_t char_asci, uint32_t target_rdev);
extern void console_put_str(char* str, uint32_t target_rdev);
extern void console_put_int_HAX(uint32_t num, uint32_t target_rdev);
extern void console_register(struct console_device* dev);

extern struct file_operations console_file_operations;
extern struct dlist console_devs;

#endif