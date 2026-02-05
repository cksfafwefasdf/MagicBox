#ifndef __DEVICE_DEVICE_H
#define __DEVICE_DEVICE_H

#define MAX_DEV_NR 32 // 包括所有的分区在内
#define MAX_DEV_NAME_LEN 64


// 将主次设备号拼接成一个 uint32_t
#define MAKEDEV(major, minor) ((((major) & 0xffff) << 16) | ((minor) & 0xffff))

// 提取主设备号
#define MAJOR(dev) ((uint32_t)((dev) >> 16))

// 提取次设备号
#define MINOR(dev) ((uint32_t)((dev) & 0xffff))

// 块设备主设备号 (Block Device Major)
#define IDE_MAJOR         3    // IDE 硬盘驱动
#define RAMDISK_MAJOR     1    // 内存盘

// 字符设备主设备号 (Char Device Major) 
#define KEYBOARD_MAJOR    1    // 键盘
#define MOUSE_MAJOR       2    // 鼠标
#define TTY_MAJOR         4    // TTY 终端
#define CONSOLE_MAJOR     5    // 控制台
#define FB_MAJOR          29   // 帧缓冲 (显卡驱动用)

#endif