#include "tty.h"
#include "print.h"
#include "ioqueue.h"
#include "console.h"
#include "timer.h"
#include "char_dev.h"
#include "device.h"
#include "signal.h"
#include "termios.h"
#include "errno.h"
#include "fs_types.h"
#include "file.h"
#include "fs.h"

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
    struct termios* term = &console_tty.termios;

    // 处理退格 (VERASE)
    if ((term->c_lflag & ICANON) && (c == term->c_cc[VERASE] || c == '\b')) {
        if (!ioq_empty(&console_tty.ibuf)) {
            uint32_t last_idx = (console_tty.ibuf.head - 1 + BUFSIZE) % BUFSIZE;
            if (console_tty.ibuf.buf[last_idx] != '\n') {
                ioq_popchar(&console_tty.ibuf);
                if (term->c_lflag & ECHO) console_put_char('\b');
            }
        }
        return;
    }

    // 处理信号 (ISIG + VINTR)
    if ((term->c_lflag & ISIG) && (c == term->c_cc[VINTR])) {
        // printk("front %d\n",console_tty.pgrp);
        ioq_putchar(&console_tty.ibuf, c);
        if(console_tty.pgrp!=0){
            kill_pgrp(console_tty.pgrp, SIGINT);
        }
        sema_signal(&console_tty.line_sem);
        return;
    }

    // 处理清行/清屏 (VKILL / VCLR)
    if (c == term->c_cc[VKILL] || c == term->c_cc[VCLR]) {
        while (!ioq_empty(&console_tty.ibuf)) {
            uint32_t last_idx = (console_tty.ibuf.head - 1 + BUFSIZE) % BUFSIZE;
            if (console_tty.ibuf.buf[last_idx] == '\n') break;
            ioq_popchar(&console_tty.ibuf);
            if (term->c_lflag & ECHO) console_put_char('\b');
        }
        if (c == term->c_cc[VCLR]) {
            ioq_putchar(&console_tty.ibuf, c); // 把 ^L 放进去给 Shell 读
            sema_signal(&console_tty.line_sem);
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
        sema_signal(&console_tty.line_sem);
        return;
    }

    // 普通字符
    ioq_putchar(&console_tty.ibuf, c);
    if (term->c_lflag & ECHO) console_put_char(c);
}

int tty_read(char* buf, uint32_t count) {
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
            console_put_str("^C");
            c = '\n';
        }

        buf[i++] = c;

        // 由消费进程在拿走数据时，同步回显换行
        if (c == '\n') {
            if (term->c_lflag & ECHO) {
                console_put_char('\n');
            }
            break;
        }

        // 一旦读到行结束符，就结束本次 read
        if (c == term->c_cc[VCLR]) {
            break;
        }
    }
    
    return i;
}

int32_t tty_ioctl(struct tty_struct* tty, uint32_t cmd, uint32_t arg) {
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
            *(pid_t*)arg = tty->pgrp;
            return 0;
        case TIOCSPGRP: // Set foreground process group
            tty->pgrp = *(pid_t*)arg;
            return 0;
        default:
            return -EINVAL;
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
    console_tty.termios.c_cc[VCLR]  = 'l' - 'a' + 1;  // 0x0C, 清屏
    console_tty.termios.c_cc[VEOF]  = 'd' - 'a' + 1;  // 0x04, 产生 EOF
    console_tty.termios.c_cc[VERASE] = 'h' - 'a' + 1; // 即 ^H，擦除字符
    // 默认开启：信号处理 | 规范模式 | 回显
    console_tty.termios.c_lflag = ISIG | ICANON | ECHO;
    
    // 初始化 TTY 层专门负责“行缓冲”的信号量
    sema_init(&console_tty.line_sem, 0); 

    lock_init(&console_tty.tty_lock);

    register_char_dev(TTY_MAJOR, &tty_dev_fops, "tty");
}

int32_t sys_ioctl(int fd, uint32_t cmd, uint32_t arg) {
    if (fd >= MAX_FILES_OPEN_PER_PROC) return -EBADF; // 错误码 1：描述符越界

    // 从进程的局部 FD 表中取出全局文件表的下标
    int32_t global_fd = fd_local2global(fd);
    if (global_fd == -1) return -EBADF; // 描述符未打开的情况
    
    struct file* file = &file_table[global_fd];

    if (file->fd_inode == NULL) return -EBADF; // 错误码 2：该描述符是空的

    if (file->fd_inode->di.i_type != FT_CHAR_SPECIAL) return -ENOTTY; // 只有字符设备（TTY）支持此操作

    struct m_inode* inode = file->fd_inode;
    if (MAJOR(inode->di.i_rdev) != TTY_MAJOR) return -ENOTTY;

    // 走到这里，说明是正经的 TTY，开始工作
    return tty_ioctl(&console_tty, cmd, arg);
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