#include <tty.h>
#include <vgacon.h>
#include <ioqueue.h>
#include <console.h>
#include <timer.h>
#include <device.h>
#include <signal.h>
#include <termios.h>
#include <errno.h>
#include <fs_types.h>
#include <fs.h>
#include <ioctl.h>
#include <debug.h>
#include <stdio-kernel.h>

#define TTY_WRITE_BUF_SIZE 128

struct tty_struct console_tty;

static int32_t tty_write(struct file* file, char* buf, uint32_t count) {
    uint32_t bytes_left = count;
    uint32_t offset = 0;
    char _buf[TTY_WRITE_BUF_SIZE + 1];

    while (bytes_left > 0) {
        // 计算本次搬运的长度，不能超过 128 字节
        uint32_t chunk_size = (bytes_left > TTY_WRITE_BUF_SIZE) ? TTY_WRITE_BUF_SIZE : bytes_left;
        
        // 拷贝到内核临时缓冲区
        memcpy(_buf, buf + offset, chunk_size);
        _buf[chunk_size] = '\0'; // 确保 put_str 能找到结束符

        // 对这 128 字节（或剩余字节）进行一次性广播
        // 这样 console_acquire 和 dlist_traversal 的频率比逐字节输出降低了 128 倍
        console_put_str(_buf,file->fd_inode->i_rdev);

        offset += chunk_size;
        bytes_left -= chunk_size;
    }

    return (int32_t)count; // 符合 POSIX，返回写入的字节数
}

// 该函数会被 uart 和键盘的中断程序调用，用于处理输入
void tty_input_handler(char c, uint32_t rdev) {
    struct termios* term = &console_tty.termios;

    // 处理信号 (ISIG + VINTR)
    // 信号通常需要优先处理，无论在什么模式下
    if ((term->c_lflag & ISIG) && (c == term->c_cc[VINTR])) {
        // printk("front %d\n",console_tty.pgrp);
        ioq_putchar(&console_tty.ibuf, c);
        if(console_tty.pgrp!=0){
            kill_pgrp(console_tty.pgrp, SIGINT);
        }
        // 信号发生时，必须唤醒正在等待输入的进程
        sema_signal(&console_tty.line_sem);
        return;
    }

    // 规范模式 (ICANON) 的特殊行编辑处理
    if (term->c_lflag & ICANON) {
        // 处理退格 (VERASE)
        if (c == term->c_cc[VERASE] || c == '\b') {
            if (!ioq_empty(&console_tty.ibuf)) {
                uint32_t last_idx = (console_tty.ibuf.head - 1 + BUFSIZE) % BUFSIZE;
                // 规范模式下，退格不能删掉上一行的换行符
                if (console_tty.ibuf.buf[last_idx] != '\n') {
                    ioq_popchar(&console_tty.ibuf);
                    // 我们的 print.s 中会处理 \b 后的光标移动以及空格输出，但是走 uart 时就没法处理了
                    // 因为现代的linux例如ubuntu接收到\b后只会移动光标，不会输出空格
                    // 因此我们在此直接输出一个 "\b \b"，手动把相应的字符删除，让vga和uart都能处理
                    if (term->c_lflag & ECHO) {
                        console_put_str("\b \b", rdev);
                    }
                }
            }
            return;
        }

        // 处理清行，清屏幕操作不在标准中，因此我们不处理
        if (c == term->c_cc[VKILL]) {
            while (!ioq_empty(&console_tty.ibuf)) {
                uint32_t last_idx = (console_tty.ibuf.head - 1 + BUFSIZE) % BUFSIZE;
                if (console_tty.ibuf.buf[last_idx] == '\n') break;
                ioq_popchar(&console_tty.ibuf);
                if (term->c_lflag & ECHO) console_put_char('\b',rdev);
            }
            
            return;
        }

        // 处理回车
        if (c == '\n' || c == '\r') {
            // 回显换行的逻辑不要放在这里！不然会引发一些竞态问题很难处理！
            // 比如会将多个prompt打印在同一行上
            // 换行的回显应该在 tty_read 里处理
            // 这样可以确保每次读取一行时，TTY 都会处理好换行和提示符的打印，时序可以得到很好的保证
            ioq_putchar(&console_tty.ibuf, '\n');
            // 只有收到回车，规范模式的 read 才会返回
            sema_signal(&console_tty.line_sem);
            return;
        }
    }
    
    // 普通字符入队逻辑 (涵盖非规范模式) 
    // 检查缓冲区是否已满，防止溢出
    if (!ioq_full(&console_tty.ibuf)) {
        ioq_putchar(&console_tty.ibuf, c);
        
        // 回显处理
        if (term->c_lflag & ECHO) {
            console_put_char(c, rdev);
        }

        // 非规范模式，只要有字符进来，就释放信号量唤醒 read
        if (!(term->c_lflag & ICANON)) {
            sema_signal(&console_tty.line_sem);
        }
    }
}

static int tty_read(struct file* file, char* buf, uint32_t count) {
    // 首先阻塞在这里，直到 tty_input_handler 释放了一个“行信号”
    sema_wait(&console_tty.line_sem);

    struct termios* term = &console_tty.termios;
    
    uint32_t i = 0;
    while (i < count) {
        // 此时它一定不会阻塞，因为信号量已经确认了缓冲区里至少有一行数据
        char c = ioq_getchar(&console_tty.ibuf);
        
        // 如果读到的是 ctrl+c 打印相应的操作，但是不要直接将这个转义字符入队
        // 而是入队一个 \n 来作为行结束标志，到时候shell取到的就是空指令了
        if (c == term->c_cc[VINTR]&&(term->c_lflag & ECHO)) {
            console_put_str("^C",file->fd_inode->i_rdev);
            c = '\n';
        }

        buf[i++] = c;

        // 由消费进程在拿走数据时，同步回显换行
        if (c == '\n') {
            if (term->c_lflag & ECHO) {
                console_put_char('\n',file->fd_inode->i_rdev);
            }
            break;
        }

        // // 一旦读到行结束符，就结束本次 read
        // if (c == term->c_cc[VCLR]) {
        //     break;
        // }
    }
    
    return i;
}

static int32_t tty_ioctl(struct inode* inode, struct file* file, uint32_t cmd, uint32_t arg) {
    ASSERT(inode == file->fd_inode);
    uint32_t rdev = inode->i_rdev;

    // 确保主设备号确实是 TTY_MAJOR
    if (MAJOR(rdev) != TTY_MAJOR) {
        return -ENOTTY;
    }

    // 由于我们现在只有一个 TTY_MAJOR 设备，所以上面那个校验通过后直接赋值就行
    struct tty_struct* tty = &console_tty;

    switch (cmd) {
        case TCGETS:
            // 将内核的 termios 拷贝给用户空间
            memcpy((void*)arg, &tty->termios, sizeof(struct termios));
            return 0;
        case TCSETS:
            // 从用户空间拷贝 termios 到内核
            memcpy(&tty->termios, (void*)arg, sizeof(struct termios));
            return 0;
        case TIOCGPGRP: // Get foreground process group
            printk("TIOCGPGRP: cur_pid=%d, cur_pgid=%d, tty_pgrp=%d\n", 
            get_running_task_struct()->pid, get_running_task_struct()->pgrp, tty->pgrp);
            *(pid_t*)arg = tty->pgrp;
            return 0;
        case TIOCSPGRP: // Set foreground process group
            tty->pgrp = *(pid_t*)arg;
            return 0;
        case TIOCGWINSZ:
            // 告诉 shell 这是一个 80x25 的标准 VGA 屏幕
            struct { uint16_t row, col, xpixel, ypixel; } *ws = (void*)arg;
            ws->row = NUM_FULL_SCREEN_LINE;
            ws->col = NUM_FULL_LINE_CH;
            return 0;
        default:
            return -ENOTTY;
    }
}

void tty_init() {
    memset(&console_tty, 0, sizeof(struct tty_struct));
    
    // 初始化底层的环形队列 (它内部会初始化自己的 lock 和指针)
    ioqueue_init(&console_tty.ibuf);

    console_tty.pgrp = 0; // 初始为 0，表示当前没有任何前台用户进程组
    
    console_tty.termios.c_cc[VINTR] = 'c' - 'a' + 1;  // 0x03, SIGINT
    console_tty.termios.c_cc[VKILL] = 'u' - 'a' + 1;  // 0x15, 清除当前行
    // 虽然内核只是把 ctrl+l 转义后放进缓冲区，但定义好它的值可以方便以后维护
    // console_tty.termios.c_cc[VCLR]  = 'l' - 'a' + 1;  // 0x0C, 清屏
    console_tty.termios.c_cc[VEOF]  = 'd' - 'a' + 1;  // 0x04, 产生 EOF
    console_tty.termios.c_cc[VERASE] = 'h' - 'a' + 1; // 即 ^H，擦除字符
    console_tty.termios.c_cc[VTIME] = 0; 
    // 默认开启：信号处理 | 规范模式 | 回显
    console_tty.termios.c_lflag = ISIG | ICANON | ECHO;
    
    // 初始化 TTY 层专门负责“行缓冲”的信号量
    sema_init(&console_tty.line_sem, 0); 

    lock_init(&console_tty.tty_lock);
}

// 适配 read
static int32_t tty_dev_read(struct inode* inode UNUSED, struct file* file, char* buf, int32_t count) {
    if(MAJOR(file->fd_inode->i_rdev) != TTY_MAJOR){
        printk("tty_dev_read: this file is not a tty!\n");
        return -EINVAL;
    }
    return tty_read(file, buf, count); 
}

// 适配 write
static int32_t tty_dev_write(struct inode* inode UNUSED, struct file* file, char* buf, int32_t count) {
    if(MAJOR(file->fd_inode->i_rdev) != TTY_MAJOR){
        printk("tty_dev_read: this file is not a tty!\n");
        return -EINVAL;
    }
    return tty_write(file, buf, count);
}

struct file_operations tty_file_operations = {
	.lseek 		= NULL,
	.read 		= tty_dev_read,
	.write 		= tty_dev_write,
	.readdir 	= NULL,
	.ioctl 		= tty_ioctl,
	.open 		= NULL,
	.release 	= NULL,
    .mmap		= NULL
};