#include "tty.h"
#include "print.h"
#include "ioqueue.h"
#include "console.h"
#include "timer.h"
#include "char_dev.h"
#include "device.h"

struct tty_struct console_tty;

struct file_operations tty_dev_fops = {
    .read = tty_dev_read,
    .write = tty_dev_write,
    .open = NULL,
    .close = NULL
};

int32_t tty_write(const char* buf, uint32_t count) {
    uint32_t i = 0;
    while (i < count) {
        console_put_char(buf[i]);
        i++;
    }
    return (int32_t)i; // 符合 POSIX，返回写入的字节数
}

void tty_input_handler(char c) {
    if (c == '\b') {
		// 只有当 head 还没退到 tail 的时候，才允许退格 (环形缓冲区是)
        // 因为 tail 之后的数据是属于“当前行”还没被 read 走的部分
        // 如果 head == tail，说明当前行已经空了，再退就要删提示符了
        if (!ioq_empty(&console_tty.ibuf)) {
            
            // 额外保险，如果上一个字符是换行符，也不允许删
            // 防止用户在第二行开头退格删掉了第一行的换行符
			// 这是用于处理短时间内输入多行数据时的情况
            char last_char = console_tty.ibuf.buf[(console_tty.ibuf.head - 1 + BUFSIZE) % BUFSIZE];
            if (last_char != '\n') {
                ioq_popchar(&console_tty.ibuf);
                console_put_char('\b');
            }
        }
    } else if (c == ('u' - 'a' + 1) || c == ('l' - 'a' + 1)) { // Ctrl+U 和 ctrl + L
		// 循环 pop，直到遇到上一个 '\n' 或者缓冲区空
		while (!ioq_empty(&console_tty.ibuf)) {
			char last = console_tty.ibuf.buf[(console_tty.ibuf.head - 1 + BUFSIZE) % BUFSIZE];
			if (last == '\n') break; // 不要删掉上一行的换行符
			
			ioq_popchar(&console_tty.ibuf);
			console_put_char('\b'); // 物理擦除
		}

        if(c == ('l' - 'a' + 1)){
            ioq_putchar(&console_tty.ibuf, c);
            sema_signal(&console_tty.line_sem);
        }
	} else if(c == '\n'||c == '\r') {
        // 对 \r 进行统一处理，归一化成 \n
		c = '\n';

        // 只有不是退格键时，才存入字符并回显
        ioq_putchar(&console_tty.ibuf, c);

        // 回显换行的逻辑不要放在这里！不然会引发一些竞态问题很难处理！
        // 比如会将多个prompt打印在同一行上
        // 换行的回显应该在 tty_read 里处理
        // 这样可以确保每次读取一行时，TTY 都会处理好换行和提示符的打印，时序可以得到很好的保证
        // console_put_char(c);

        // 只有存入的是行结束符，才触发信号量
        sema_signal(&console_tty.line_sem);

    } else {
        // 只有不是退格键时，才存入字符并回显
        ioq_putchar(&console_tty.ibuf, c);
        console_put_char(c);
    }
}

int tty_read(char* buf, uint32_t count) {
    // 首先阻塞在这里，直到 tty_input_handler 释放了一个“行信号”
    sema_wait(&console_tty.line_sem);

    // 马上要对 tty buf 进行进行操作时再拿锁！不要在sema_wait操作前面拿！不然会死锁！
    
    uint32_t i = 0;
    while (i < count) {
        // 此时它一定不会阻塞，因为信号量已经确认了缓冲区里至少有一行数据
        char c = ioq_getchar(&console_tty.ibuf);
        
        buf[i++] = c;

        // 一旦读到行结束符，就结束本次 read
        if (c == '\n' || c == ('l' - 'a' + 1)) {
            break;
        }
    }
    // 读完一行后，打印一个换行符
    // 物理屏幕上的换行动作，现在与 Shell 进程获取数据的动作在时间线上强行重合
    console_put_char('\n');
    return i;
}

void tty_init() {
    memset(&console_tty, 0, sizeof(struct tty_struct));
    
    // 初始化底层的环形队列 (它内部会初始化自己的 lock 和指针)
    ioqueue_init(&console_tty.ibuf);
    
    // 初始化 TTY 层专门负责“行缓冲”的信号量
    sema_init(&console_tty.line_sem, 0); 

    lock_init(&console_tty.tty_lock);

    register_char_dev(TTY_MAJOR, &tty_dev_fops, "tty");
}

// --------------- 以下的代码用于 tty 设备适配虚拟文件系统 ---------------

// 适配 read
int32_t tty_dev_read(struct file* file, void* buf, uint32_t count) {
    return tty_read(buf, count); 
}

// 适配 write
int32_t tty_dev_write(struct file* file, const void* buf, uint32_t count) {
    return tty_write(buf, count);
}

// 初始化：将 TTY 注册为 0 号字符设备
void tty_dev_init() {
    register_char_dev(0, &tty_dev_fops, "tty");
}