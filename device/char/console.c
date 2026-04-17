#include <console.h>
#include <vgacon.h>
#include <sync.h>
#include <thread.h>
#include <device.h>
#include <fs_types.h>
#include <dlist.h>
#include <errno.h>
#include <debug.h>

static struct lock console_lock;

struct console_callback_info {
    uint32_t rdev;
    void (*handler)(struct console_device* dev, struct console_callback_info* info);
    union {
        char* str;
        uint32_t num;
        char ascii_char;
    };
};

// 控制台设备链表，以后我们注册控制台设备的 inode 的话就可以通过遍历这个链表来注册了
// 而不是在相应的注册函数里面硬编码，这样的方法是合理的，
// 因为在文件系统看来，tty0 和 ttyS0 没什么区别，都是 tty 设备
// 调用的都是 tty_read 和 tty_write，只不过在 tty_write 中
// 走到具体的 console_put_str 的时候在 console 层才会进行链表的遍历或者分发
struct dlist console_devs;

// 打印字符串的处理逻辑
static void _h_put_str(struct console_device* dev, struct console_callback_info* info) {
    if (dev->put_str) dev->put_str(info->str);
}

// 打印数字的处理逻辑
static void _h_put_int(struct console_device* dev, struct console_callback_info* info) {
    if (dev->put_int) dev->put_int(info->num);
}

// 打印字符的处理逻辑
static void _h_put_char(struct console_device* dev, struct console_callback_info* info) {
    if (dev->put_char) dev->put_char(info->ascii_char);
}
// 用于打印操作的分发
static bool _cb_put_dispatch(struct dlist_elem* elem, void* arg) {
    struct console_device* dev = member_to_entry(struct console_device, dev_tag, elem);
    struct console_callback_info* info = (struct console_callback_info*)arg;

    // 如果是广播，或者设备号匹配
    if (info->rdev == BROADCAST_RDEV || dev->rdev == info->rdev) {
        // 调用我们传入的具体处理函数
        if (info->handler) {
            info->handler(dev, info);
        }

        // 如果是点对点匹配成功，返回 true 提前结束遍历
        if (info->rdev != BROADCAST_RDEV) return true;
    }

    return false; // 继续遍历（广播模式下会一直跑到尾部）
}

// 适配 VFS 的写函数
static int32_t console_dev_write(struct inode* inode UNUSED,struct file* file UNUSED, char* buf, int32_t count) {
    const char* data = buf;
    int32_t i = 0;
    while (i < count) {
        console_put_char(data[i],BROADCAST_RDEV);
        i++;
    }
    return i; // 返回写入的字节数，以便于符合 POSIX
}

void console_put_str(char* str,uint32_t target_rdev) {
    struct console_callback_info info = {
        .rdev = target_rdev,
        .handler = _h_put_str,
        .str = str
    };

    console_acquire();
    dlist_traversal(&console_devs, _cb_put_dispatch, &info);
    console_release();
}

void console_put_char(uint8_t ascii_char, uint32_t target_rdev){
    struct console_callback_info info = {
        .rdev = target_rdev,
        .handler = _h_put_char,
        .ascii_char = ascii_char
    };

    console_acquire();
    dlist_traversal(&console_devs, _cb_put_dispatch, &info);
    console_release();
}

void console_put_int_HAX(uint32_t num,uint32_t target_rdev){
    struct console_callback_info info = {
        .rdev = target_rdev,
        .handler = _h_put_int,
        .num = num
    };

    console_acquire();
    dlist_traversal(&console_devs, _cb_put_dispatch, &info);
    console_release();
}

// 该函数通常在初始化的早期调用，因此可以先不加锁
void console_register(struct console_device* dev) {
    dlist_push_back(&console_devs,&dev->dev_tag);
}

void console_init(void){
	lock_init(&console_lock);
    dlist_init(&console_devs);
}

void console_acquire(void){
	lock_acquire(&console_lock);
}

void console_release(void){
	lock_release(&console_lock);
}

struct file_operations console_file_operations = {
	.lseek 		= NULL,
	.read 		= NULL,
	.write 		= console_dev_write,
	.readdir 	= NULL,
	.ioctl 		= NULL,
	.open 		= NULL,
	.release 	= NULL,
	.mmap		= NULL,
    .poll		= NULL
};