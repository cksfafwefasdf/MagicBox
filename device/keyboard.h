#ifndef __DEVICE_KEYBOARD_H
#define __DEVICE_KEYBOARD_H
extern void keyboard_init(void);
extern struct ioqueue kbd_buf; // keyboard buffer is a public resource
#endif