#include <vgacon.h>
#include <console.h>
#include <string.h>
#include <device.h>

// vga 模式下的控制台相关的操作

struct console_device console_vgacon;

void vgacon_init(){
    memset(&console_vgacon, 0, sizeof(struct console_device));
    console_vgacon.put_char = put_char;
    console_vgacon.put_str = put_str;
    console_vgacon.put_int = put_int;
    console_vgacon.rdev = MAKEDEV(TTY_MAJOR, TTY0_MINOR);
    memset(console_vgacon.name,0,CONSOLE_DEV_NAME_LEN);
    strcpy(console_vgacon.name,"tty0");
    console_register(&console_vgacon);
}

