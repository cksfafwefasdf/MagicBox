#include <console.h>
#include <print.h>
#include <sync.h>
#include <thread.h>
#include <device.h>
#include <char_dev.h>
#include <fs_types.h>

static struct lock console_lock;

// 适配 VFS 的写函数
static int32_t console_dev_write(struct inode* inode UNUSED,struct file* file UNUSED, char* buf, int32_t count) {
    const char* data = buf;
    int32_t i = 0;
    while (i < count) {
        // 直接调用封装好的带锁函数
        console_put_char(data[i]);
        i++;
    }
    return i; // 返回写入的字节数，以便于符合 POSIX
}

void console_init(void){
	lock_init(&console_lock);
}

void console_acquire(void){
	lock_acquire(&console_lock);
}

void console_release(void){
	lock_release(&console_lock);
}

void console_put_str(char* str){
	console_acquire();
	put_str(str);
	console_release();
}


void console_put_char(uint8_t char_asci){
	console_acquire();
	put_char(char_asci);
	console_release();
}

void console_put_int_HAX(uint32_t num){
	console_acquire();
	put_int(num);
	console_release();
}

struct file_operations console_file_operations = {
	.lseek 		= NULL,
	.read 		= NULL,
	.write 		= console_dev_write,
	.readdir 	= NULL,
	.ioctl 		= NULL,
	.open 		= NULL,
	.release 	= NULL
};